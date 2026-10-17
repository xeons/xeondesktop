/* SPDX-License-Identifier: MIT
 * xd-desk: Display Properties.
 *
 * Tabs follow Windows 2000: Background, Screen Saver, Appearance, Effects,
 * Settings. Changes are kept in `pending` and written to desktop.ini on
 * Apply/OK; the compositor and shell pick the file up immediately.
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

#include "../core/alloc.h"
#include "../saver/savers.h"
#include "../settings/settings.h"
#include "../ui/image.h"
#include "../ui/ui.h"
#include "common.h"

enum { PAGE_BACKGROUND, PAGE_SCREENSAVER, PAGE_APPEARANCE, PAGE_EFFECTS, PAGE_SETTINGS };

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
    int chooser_fd;
    xd_ui_source *chooser_src;
    xd_strbuf chooser_out;

    /* Screen Saver */
    xd_widget *saver_preview;
    xd_widget *saver_combo;
    xd_widget *saver_preview_btn;
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
    xd_widget *style_combo;
    xd_widget *scheme_combo;
    xd_widget *font_combo;
    xd_widget *appearance_preview;

    /* Effects */
    xd_widget *smoothing_check;
    xd_widget *smoothing_combo;

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

static void changed(desk *d) {
    xd_widget_set_enabled(d->apply,
                          d->display_dirty || !xd_settings_equal(&d->saved, &d->pending));
    xd_widget_redraw(d->apply);
}

static void update_animation(desk *d);

static void on_page(xd_widget *w, void *user) {
    desk *d = user;
    d->page = xd_widget_selected(w);
    update_animation(d);
}

/* Pending appearance as a theme for previews. */
static void pending_theme(const desk *d, double scale, xd_theme *t) {
    xd_scheme scheme;
    xd_settings_scheme(&d->pending, &scheme);
    xd_theme_init(t, d->pending.style, &scheme, scale);
}

/* A Windows 2000-style monitor: beige body, dark screen bezel, stand.
 * Returns the screen rect for the caller to fill. */
static xd_rect draw_monitor(cairo_t *cr, xd_rect r, const xd_theme *t, double aspect) {
    int px = xd_tpx(t, 1);
    int stand_h = xd_tpx(t, 18);
    xd_rect body = {r.x, r.y, r.w, r.h - stand_h};
    /* Keep the screen's aspect ratio: shrink the body to fit. */
    int bezel = xd_tpx(t, 10);
    int sw = body.w - 2 * bezel;
    int sh = (int)lround(sw / aspect);
    if (sh > body.h - 2 * bezel) {
        sh = body.h - 2 * bezel;
        sw = (int)lround(sh * aspect);
    }
    body.w = sw + 2 * bezel;
    body.h = sh + 2 * bezel;
    body.x = r.x + (r.w - body.w) / 2;

    xd_rgba face = xd_rgba_hex(0xD4D0C8);
    xd_rgba light = xd_rgba_hex(0xFFFFFF);
    xd_rgba shadow = xd_rgba_hex(0x808080);
    xd_rgba dark = xd_rgba_hex(0x404040);
    /* Stand: neck and foot. */
    int neck_w = body.w / 5;
    xd_rect neck = {body.x + (body.w - neck_w) / 2, body.y + body.h, neck_w, stand_h / 2};
    xd_rect foot = {body.x + body.w / 4, neck.y + neck.h, body.w / 2, stand_h / 2};
    xd_fill_rect(cr, neck, face);
    xd_draw_bevel(cr, neck, px, light, dark);
    xd_fill_rect(cr, foot, face);
    xd_draw_bevel(cr, foot, px, light, dark);

    xd_fill_rect(cr, body, face);
    xd_rect in = xd_draw_bevel(cr, body, px, light, dark);
    xd_draw_bevel(cr, in, px, face, shadow);
    xd_rect screen = {body.x + bezel, body.y + bezel, sw, sh};
    xd_draw_bevel(cr, xd_rect_inset(screen, -px, -px), px, shadow, light);
    /* Power light. */
    xd_fill_rect(cr, (xd_rect){body.x + body.w - bezel - xd_tpx(t, 4), body.y + body.h - bezel / 2 - px,
                               xd_tpx(t, 3), xd_tpx(t, 2)},
                 xd_rgba_hex(0x30C030));
    return screen;
}

static double primary_aspect(desk *d) {
    const xd_output_info *outs = nullptr;
    size_t n = xd_ui_outputs(d->ui, &outs);
    if (n > 0 && outs[0].logical_w > 0 && outs[0].logical_h > 0)
        return (double)outs[0].logical_w / outs[0].logical_h;
    return 4.0 / 3.0;
}

/* ---- Background ----------------------------------------------------------------- */

static bool is_image(xd_str name) {
    static const char *const exts[] = {".jpg", ".jpeg", ".png", ".webp", ".bmp"};
    for (size_t i = 0; i < XD_ARRAY_LEN(exts); i++) {
        xd_str e = xd_str_from_cstr(exts[i]);
        if (name.len > e.len && xd_str_eq_nocase(xd_str_sub(name, name.len - e.len, e.len), e))
            return true;
    }
    return false;
}

static void add_wallpaper(desk *d, const char *path) {
    for (size_t i = 1; i < d->wallpaper_count; i++) {
        if (strcmp(d->wallpapers[i], path) == 0)
            return;
    }
    if (d->wallpaper_count == d->wallpaper_cap) {
        d->wallpaper_cap = d->wallpaper_cap == 0 ? 64 : d->wallpaper_cap * 2;
        d->wallpapers = xd_realloc_array(d->wallpapers, d->wallpaper_cap, sizeof(char *));
    }
    d->wallpapers[d->wallpaper_count++] = xd_str_dup_cstr(xd_str_from_cstr(path));
}

/* Scans a wallpaper directory (two levels: themes keep images in subdirs). */
static void scan_wallpapers(desk *d, const char *dir, int depth) {
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
            scan_wallpapers(d, xd_strbuf_cstr(&path), depth + 1);
        else if (S_ISREG(st.st_mode) && is_image(name))
            add_wallpaper(d, xd_strbuf_cstr(&path));
    }
    xd_strbuf_free(&path);
    closedir(dh);
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

static int compare_wallpapers(const void *a, const void *b) {
    return strcoll(base_name(*(char *const *)a), base_name(*(char *const *)b));
}

static void load_wallpapers(desk *d) {
    add_wallpaper(d, ""); /* (None) */
    static const char *const dirs[] = {"/usr/share/backgrounds", "/usr/share/wallpapers",
                                       "/usr/local/share/backgrounds"};
    for (size_t i = 0; i < XD_ARRAY_LEN(dirs); i++)
        scan_wallpapers(d, dirs[i], 0);
    const char *home = getenv("HOME");
    if (home != nullptr) {
        xd_strbuf p = {0};
        xd_strbuf_appendf(&p, "%s/Pictures/Wallpapers", home);
        scan_wallpapers(d, xd_strbuf_cstr(&p), 1);
        xd_strbuf_free(&p);
    }
    /* Keep the current one listed even if it lives elsewhere. */
    if (d->pending.wallpaper[0] != '\0')
        add_wallpaper(d, d->pending.wallpaper);
    if (d->wallpaper_count > 2)
        qsort(d->wallpapers + 1, d->wallpaper_count - 1, sizeof(char *), compare_wallpapers);
}

static void draw_bg_preview(xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t, void *user) {
    (void)w;
    desk *d = user;
    xd_rect screen = draw_monitor(cr, r, t, primary_aspect(d));
    xd_theme pt;
    pending_theme(d, t->scale, &pt);
    xd_fill_rect(cr, screen, xd_sys(&pt, XD_COLOR_BACKGROUND));

    if (strcmp(d->preview_path, d->pending.wallpaper) != 0) {
        if (d->preview_image != nullptr)
            cairo_surface_destroy(d->preview_image);
        d->preview_image = d->pending.wallpaper[0] != '\0'
                               ? xd_image_load(d->pending.wallpaper, 480, 360)
                               : nullptr;
        int full_h;
        if (!xd_image_size(d->pending.wallpaper, &d->preview_full_w, &full_h))
            d->preview_full_w = 0;
        snprintf(d->preview_path, sizeof(d->preview_path), "%s", d->pending.wallpaper);
    }
    /* Center and Tile show the picture at its real size relative to the
     * screen; the preview copy was loaded smaller, so scale it back up. */
    const xd_output_info *outs = nullptr;
    double scale = 0.25;
    if (xd_ui_outputs(d->ui, &outs) > 0 && outs[0].mode_w > 0)
        scale = (double)screen.w / outs[0].mode_w;
    if (d->preview_image != nullptr && d->preview_full_w > 0)
        scale *= (double)d->preview_full_w / cairo_image_surface_get_width(d->preview_image);
    xd_image_paint_wallpaper(cr, d->preview_image, screen, d->pending.wallpaper_mode, scale);
}

static void on_wallpaper(xd_widget *w, void *user) {
    desk *d = user;
    int i = xd_widget_selected(w);
    if (i < 0 || (size_t)i >= d->wallpaper_count)
        return;
    snprintf(d->pending.wallpaper, sizeof(d->pending.wallpaper), "%s", d->wallpapers[i]);
    xd_widget_redraw(d->bg_preview);
    changed(d);
}

static void on_wallpaper_mode(xd_widget *w, void *user) {
    desk *d = user;
    int i = xd_widget_selected(w);
    if (i >= 0 && i < XD_WALLPAPER_MODE_COUNT)
        d->pending.wallpaper_mode = (xd_wallpaper_mode)i;
    xd_widget_redraw(d->bg_preview);
    changed(d);
}


/* Selects a picture by path, adding it to the list if it's new. */
static void choose_wallpaper(desk *d, const char *path) {
    size_t before = d->wallpaper_count;
    add_wallpaper(d, path);
    if (d->wallpaper_count > before)
        xd_widget_add_item(d->wallpaper_list, base_name(path));
    for (size_t i = 0; i < d->wallpaper_count; i++) {
        if (strcmp(d->wallpapers[i], path) == 0)
            xd_widget_select(d->wallpaper_list, (int)i);
    }
    snprintf(d->pending.wallpaper, sizeof(d->pending.wallpaper), "%s", path);
    xd_widget_redraw(d->bg_preview);
    changed(d);
}

static void chooser_output(void *user) {
    desk *d = user;
    char buf[1024];
    ssize_t n = read(d->chooser_fd, buf, sizeof(buf));
    if (n > 0) {
        xd_strbuf_append(&d->chooser_out, (xd_str){buf, (size_t)n});
        return;
    }
    if (n < 0 && errno == EINTR)
        return;
    /* The chooser closed: its answer is complete. */
    xd_ui_remove_source(d->chooser_src);
    d->chooser_src = nullptr;
    close(d->chooser_fd);
    d->chooser_fd = -1;
    xd_str path = xd_str_trim(xd_strbuf_view(&d->chooser_out));
    if (path.len > 0) {
        char *p = xd_str_dup_cstr(path);
        choose_wallpaper(d, p);
        xd_free(p);
    }
    xd_strbuf_clear(&d->chooser_out);
    xd_widget_set_enabled(d->browse, true);
}

static void on_browse([[maybe_unused]] xd_widget *w, void *user) {
    desk *d = user;
    if (d->chooser_src != nullptr)
        return;
    char folder[1024] = "/usr/share/backgrounds";
    const char *home = getenv("HOME");
    if (home != nullptr)
        snprintf(folder, sizeof(folder), "%s/Pictures", home);
    const char *args[] = {"--images", "--title", "Browse", "--folder", folder, nullptr};
    int fd = -1;
    if (xd_spawn_own("xd-choose-file", args, &fd) == 0)
        return;
    d->chooser_fd = fd;
    d->chooser_src = xd_ui_add_fd(d->ui, fd, chooser_output, d);
    xd_widget_set_enabled(d->browse, false);
}

static void build_background(desk *d) {
    xd_window *win = d->win;
    d->bg_preview = xd_custom(win, (xd_rect){105, 42, 194, 166}, draw_bg_preview, d);
    xd_widget_set_page(d->bg_preview, PAGE_BACKGROUND);
    xd_widget_set_page(xd_label(win, (xd_rect){20, 218, 360, 16},
                                "Select a picture to use as wallpaper:", 0),
                       PAGE_BACKGROUND);
    d->wallpaper_list = xd_listbox(win, (xd_rect){20, 236, 268, 160}, on_wallpaper, d);
    xd_widget_set_page(d->wallpaper_list, PAGE_BACKGROUND);
    int current = 0;
    for (size_t i = 0; i < d->wallpaper_count; i++) {
        const char *p = d->wallpapers[i];
        xd_widget_add_item(d->wallpaper_list, p[0] == '\0' ? "(None)" : base_name(p));
        if (strcmp(p, d->pending.wallpaper) == 0)
            current = (int)i;
    }
    xd_widget_select(d->wallpaper_list, current);

    d->browse = xd_button(win, (xd_rect){298, 236, 86, 23}, "Browse...", on_browse, d);
    xd_widget_set_page(d->browse, PAGE_BACKGROUND);
    xd_widget_set_page(xd_label(win, (xd_rect){298, 330, 86, 16}, "Picture Display:", 0),
                       PAGE_BACKGROUND);
    d->wallpaper_mode = xd_combo(win, (xd_rect){298, 348, 86, 21}, false, on_wallpaper_mode, d);
    xd_widget_set_page(d->wallpaper_mode, PAGE_BACKGROUND);
    static const char *const modes[] = {"Center", "Tile", "Stretch", "Fill", "Fit"};
    for (size_t i = 0; i < XD_ARRAY_LEN(modes); i++)
        xd_widget_add_item(d->wallpaper_mode, modes[i]);
    xd_widget_select(d->wallpaper_mode, (int)d->pending.wallpaper_mode);
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

static const char *pending_saver(const desk *d) {
    return xd_saver_find(d->pending.screensaver) != nullptr ? d->pending.screensaver : "";
}

static void draw_saver_preview(xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t,
                               void *user) {
    (void)w;
    desk *d = user;
    xd_rect screen = draw_monitor(cr, r, t, primary_aspect(d));
    const char *id = pending_saver(d);
    if (id[0] == '\0') {
        /* (None): the desktop as it is. */
        xd_theme pt;
        pending_theme(d, t->scale, &pt);
        xd_fill_rect(cr, screen, xd_sys(&pt, XD_COLOR_BACKGROUND));
        return;
    }
    if (d->preview_saver == nullptr || strcmp(d->preview_saver_id, id) != 0 ||
        d->preview_saver_w != screen.w || d->preview_saver_h != screen.h) {
        if (d->preview_saver != nullptr)
            xd_saver_destroy(d->preview_saver);
        /* A miniature: sized as if the real screen were shrunk to fit. */
        double scale = fmax(0.2, t->scale * screen.w / 1024.0);
        d->preview_saver = xd_saver_create(id, screen.w, screen.h, scale);
        snprintf(d->preview_saver_id, sizeof(d->preview_saver_id), "%s", id);
        d->preview_saver_w = screen.w;
        d->preview_saver_h = screen.h;
        d->preview_last = (struct timespec){0};
    }
    if (d->preview_saver == nullptr)
        return;
    cairo_save(cr);
    cairo_rectangle(cr, screen.x, screen.y, screen.w, screen.h);
    cairo_clip(cr);
    cairo_translate(cr, screen.x, screen.y);
    xd_saver_frame(d->preview_saver, cr, seconds_since(&d->preview_last));
    cairo_restore(cr);
}

static void animate(void *user) {
    desk *d = user;
    xd_widget_redraw(d->saver_preview);
}

/* The preview runs only while its page shows. */
static void update_animation(desk *d) {
    bool want = d->page == PAGE_SCREENSAVER && xd_saver_animated(pending_saver(d));
    if (want && d->anim == nullptr)
        d->anim = xd_ui_add_timer(d->ui, 50, true, animate, d);
    else if (!want && d->anim != nullptr) {
        xd_ui_remove_source(d->anim);
        d->anim = nullptr;
    }
}

static const int wait_minutes[] = {1, 2, 3, 5, 10, 15, 20, 25, 30, 45, 60, 90, 120, 180, 240};
static const int monitor_minutes[] = {0, 1, 2, 3, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180, 240, 300};

static void saver_controls(desk *d) {
    bool on = pending_saver(d)[0] != '\0';
    xd_widget_set_enabled(d->saver_preview_btn, on);
    xd_widget_set_enabled(d->saver_password, on);
    xd_widget_set_enabled(d->saver_wait, on);
}

static void on_saver(xd_widget *w, void *user) {
    desk *d = user;
    int i = xd_widget_selected(w);
    const xd_saver_info *savers = nullptr;
    size_t n = xd_savers(&savers);
    if (i <= 0 || (size_t)(i - 1) >= n)
        d->pending.screensaver[0] = '\0';
    else
        snprintf(d->pending.screensaver, sizeof(d->pending.screensaver), "%s", savers[i - 1].id);
    saver_controls(d);
    update_animation(d);
    xd_widget_redraw(d->saver_preview);
    changed(d);
}

static void on_saver_password(xd_widget *w, void *user) {
    desk *d = user;
    d->pending.screensaver_password = xd_checkbox_checked(w);
    changed(d);
}

static void on_saver_wait(xd_widget *w, void *user) {
    desk *d = user;
    int i = xd_widget_selected(w);
    if (i >= 0 && (size_t)i < d->wait_count)
        d->pending.screensaver_wait = d->wait_values[i];
    changed(d);
}

static void on_monitor_off(xd_widget *w, void *user) {
    desk *d = user;
    int i = xd_widget_selected(w);
    if (i >= 0 && (size_t)i < XD_ARRAY_LEN(monitor_minutes))
        d->pending.monitor_off = monitor_minutes[i];
    changed(d);
}

static void on_saver_preview_button([[maybe_unused]] xd_widget *w, void *user) {
    desk *d = user;
    const char *id = pending_saver(d);
    if (id[0] == '\0')
        return;
    /* Preview runs unprotected, like Windows' Preview button. */
    const char *args[] = {"--saver", id, "--no-lock", nullptr};
    xd_spawn_own("xd-lock", args, nullptr);
}

static const char *minutes_label(int m, char *buf, size_t cap) {
    if (m == 0)
        snprintf(buf, cap, "Never");
    else if (m < 60)
        snprintf(buf, cap, "After %d min%s", m, m == 1 ? "" : "s");
    else
        snprintf(buf, cap, "After %d hour%s", m / 60, m == 60 ? "" : "s");
    return buf;
}

static void build_screensaver(desk *d) {
    xd_window *win = d->win;
    const int P = PAGE_SCREENSAVER;
    d->saver_preview = xd_custom(win, (xd_rect){105, 42, 194, 166}, draw_saver_preview, d);
    xd_widget_set_page(d->saver_preview, P);
    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 216, 364, 92}, "Screen Saver"), P);
    d->saver_combo = xd_combo(win, (xd_rect){32, 238, 170, 21}, false, on_saver, d);
    xd_widget_set_page(d->saver_combo, P);
    xd_widget_add_item(d->saver_combo, "(None)");
    const xd_saver_info *savers = nullptr;
    size_t n = xd_savers(&savers);
    int current = 0;
    for (size_t i = 0; i < n; i++) {
        xd_widget_add_item(d->saver_combo, savers[i].name);
        if (strcmp(savers[i].id, d->pending.screensaver) == 0)
            current = (int)i + 1;
    }
    xd_widget_select(d->saver_combo, current);

    /* The savers have no options of their own yet. */
    xd_widget *settings = xd_button(win, (xd_rect){210, 237, 80, 23}, "Settings", nullptr, nullptr);
    xd_widget_set_enabled(settings, false);
    xd_widget_set_page(settings, P);
    d->saver_preview_btn =
        xd_button(win, (xd_rect){296, 237, 80, 23}, "Preview", on_saver_preview_button, d);
    xd_widget_set_page(d->saver_preview_btn, P);
    d->saver_password = xd_checkbox(win, (xd_rect){32, 272, 150, 20}, "Password protected",
                                    d->pending.screensaver_password, on_saver_password, d);
    xd_widget_set_page(d->saver_password, P);

    xd_widget_set_page(xd_label(win, (xd_rect){202, 274, 32, 16}, "Wait:", 0), P);
    d->saver_wait = xd_combo(win, (xd_rect){236, 271, 82, 21}, false, on_saver_wait, d);
    xd_widget_set_page(d->saver_wait, P);
    /* The usual choices, plus whatever desktop.ini has. */
    d->wait_count = 0;
    bool listed = false;
    for (size_t i = 0; i < XD_ARRAY_LEN(wait_minutes); i++) {
        if (!listed && wait_minutes[i] > d->pending.screensaver_wait) {
            d->wait_values[d->wait_count++] = d->pending.screensaver_wait;
            listed = true;
        }
        if (wait_minutes[i] == d->pending.screensaver_wait)
            listed = true;
        d->wait_values[d->wait_count++] = wait_minutes[i];
    }
    if (!listed)
        d->wait_values[d->wait_count++] = d->pending.screensaver_wait;
    int wait_sel = 0;
    for (size_t i = 0; i < d->wait_count; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", d->wait_values[i]);
        xd_widget_add_item(d->saver_wait, buf);
        if (d->wait_values[i] == d->pending.screensaver_wait)
            wait_sel = (int)i;
    }
    xd_widget_select(d->saver_wait, wait_sel);
    xd_widget_set_page(xd_label(win, (xd_rect){324, 274, 56, 16}, "minutes", 0), P);

    xd_widget_set_page(
        xd_groupbox(win, (xd_rect){20, 316, 364, 64}, "Energy saving features of monitor"), P);
    xd_widget_set_page(
        xd_image(win, (xd_rect){32, 338, 32, 32}, "preferences-system-power-management"), P);
    xd_widget_set_page(xd_label(win, (xd_rect){74, 342, 110, 16}, "Turn off monitor:", 0), P);
    xd_widget *monitor = xd_combo(win, (xd_rect){186, 339, 150, 21}, false, on_monitor_off, d);
    xd_widget_set_page(monitor, P);
    int mon_sel = 0;
    int best = INT32_MAX;
    for (size_t i = 0; i < XD_ARRAY_LEN(monitor_minutes); i++) {
        char buf[32];
        xd_widget_add_item(monitor, minutes_label(monitor_minutes[i], buf, sizeof(buf)));
        int diff = abs(monitor_minutes[i] - d->pending.monitor_off);
        if (diff < best) {
            best = diff;
            mon_sel = (int)i;
        }
    }
    xd_widget_select(monitor, mon_sel);
    saver_controls(d);
}

static xd_widget *on_page_disabled(xd_widget *w, int page) {
    xd_widget_set_enabled(w, false);
    xd_widget_set_page(w, page);
    return w;
}

/* ---- Appearance ---------------------------------------------------------------- */

static void draw_appearance_preview(xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t,
                                    void *user) {
    (void)w;
    desk *d = user;
    xd_theme pt;
    pending_theme(d, t->scale, &pt);
    int px = xd_tpx(t, 1);

    /* Sunken well like Windows' preview box. */
    xd_rect well = xd_theme_draw_field(t, cr, r, false);
    xd_fill_rect(cr, well, xd_sys(&pt, XD_COLOR_BACKGROUND));

    /* Inactive window behind, active window in front, message box on top. */
    xd_frame inactive = {.title = "Inactive Window",
                         .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE};
    xd_rect r1 = {well.x + xd_tpx(t, 10), well.y + xd_tpx(t, 8), well.w - xd_tpx(t, 60),
                  xd_tpx(t, 90)};
    xd_theme_draw_frame(&pt, cr, r1, &inactive);
    xd_insets in1 = xd_theme_frame_insets(&pt, &inactive);
    xd_fill_rect(cr, xd_rect_inset4(r1, in1.left, in1.top, in1.right, in1.bottom),
                 xd_theme_dialog_color(&pt));

    xd_frame active = {.title = "Active Window",
                       .active = true,
                       .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE};
    xd_rect r2 = {well.x + xd_tpx(t, 24), well.y + xd_tpx(t, 36), well.w - xd_tpx(t, 48),
                  well.h - xd_tpx(t, 44)};
    xd_theme_draw_frame(&pt, cr, r2, &active);
    xd_insets in2 = xd_theme_frame_insets(&pt, &active);
    xd_rect client = xd_rect_inset4(r2, in2.left, in2.top, in2.right, in2.bottom);
    xd_fill_rect(cr, client, xd_theme_dialog_color(&pt));
    xd_rect text_area = xd_rect_inset(client, xd_tpx(&pt, 6), xd_tpx(&pt, 6));
    xd_rect field = {text_area.x, text_area.y, text_area.w / 2, xd_tpx(&pt, 40)};
    xd_rect field_in = xd_theme_draw_field(&pt, cr, field, false);
    xd_text_style st = xd_text(&pt, XD_FONT_MESSAGE, xd_sys(&pt, XD_COLOR_WINDOW_TEXT),
                               XD_ALIGN_START);
    xd_draw_text(cr, xd_rect_inset4(field_in, xd_tpx(&pt, 3), 0, 0, 0), "Window Text", &st);

    xd_frame box = {.title = "Message Box", .active = true, .dialog = true,
                    .buttons = XD_CAPBTN_CLOSE};
    xd_insets ib = xd_theme_frame_insets(&pt, &box);
    int bw = xd_tpx(&pt, 150);
    int bh = ib.top + ib.bottom + xd_tpx(&pt, 52);
    xd_rect r3 = {client.x + client.w - bw - xd_tpx(&pt, 8),
                  XD_MIN(client.y + xd_tpx(&pt, 8), well.y + well.h - bh - px), bw, bh};
    xd_theme_draw_frame(&pt, cr, r3, &box);
    xd_rect bc = xd_rect_inset4(r3, ib.left, ib.top, ib.right, ib.bottom);
    xd_fill_rect(cr, bc, xd_theme_dialog_color(&pt));
    xd_draw_text(cr, (xd_rect){bc.x + xd_tpx(&pt, 8), bc.y + xd_tpx(&pt, 4), bc.w, xd_tpx(&pt, 16)},
                 "Message Text", &st);
    int okw = xd_tpx(&pt, 60);
    xd_theme_draw_button(&pt, cr,
                         (xd_rect){bc.x + (bc.w - okw) / 2, bc.y + xd_tpx(&pt, 24), okw,
                                   xd_tpx(&pt, 22)},
                         XD_STATE_DEFAULT, "OK");
}

/* Which Windows XP style colour variant installed scheme `i` is, if any. */
static xd_luna_color scheme_luna_color(const desk *d, size_t i) {
    xd_settings probe;
    xd_settings_defaults(&probe);
    snprintf(probe.scheme, sizeof(probe.scheme), "%s", d->schemes.stems[i]);
    xd_scheme scheme;
    xd_settings_scheme(&probe, &scheme);
    return scheme.luna_color;
}

/* As in XP, the Color scheme list holds the colour variants of Windows XP
 * style (Default (blue), Olive Green, Silver) or, for the other styles, the
 * classic schemes after the built-in Windows Standard. */
static bool scheme_listed(const desk *d, size_t i) {
    return (scheme_luna_color(d, i) != XD_LUNA_NONE) == (d->pending.style == XD_STYLE_LUNA);
}

/* The installed scheme shown as combo item `item`, or -1 for the built-in
 * Windows Standard. */
static ptrdiff_t listed_scheme(const desk *d, int item) {
    int n = d->pending.style == XD_STYLE_LUNA ? 0 : 1;
    for (size_t i = 0; i < d->schemes.count; i++) {
        if (scheme_listed(d, i) && n++ == item)
            return (ptrdiff_t)i;
    }
    return -1;
}

static void fill_schemes(desk *d) {
    xd_widget_clear_items(d->scheme_combo);
    bool luna = d->pending.style == XD_STYLE_LUNA;
    if (!luna)
        xd_widget_add_item(d->scheme_combo, "Windows Standard (built in)");
    int n = luna ? 0 : 1;
    int current = 0;
    for (size_t i = 0; i < d->schemes.count; i++) {
        if (!scheme_listed(d, i))
            continue;
        xd_widget_add_item(d->scheme_combo, d->schemes.names[i]);
        /* A classic scheme left under Windows XP style shows as Default
         * (blue), which is what its frames look like. */
        if (strcmp(d->schemes.stems[i], d->pending.scheme) == 0 ||
            (luna && current == 0 && scheme_luna_color(d, i) == XD_LUNA_BLUE))
            current = n;
        n++;
    }
    xd_widget_select(d->scheme_combo, current);
}

static void on_style(xd_widget *w, void *user) {
    desk *d = user;
    int i = xd_widget_selected(w);
    if (i < 0 || i >= XD_STYLE_COUNT || (xd_visual_style)i == d->pending.style)
        return;
    bool was_luna = d->pending.style == XD_STYLE_LUNA;
    d->pending.style = (xd_visual_style)i;
    /* Switching between Windows XP style and the others switches scheme
     * lists, and the scheme to the new list's first: Default (blue), or
     * Windows Standard. */
    if (was_luna != (d->pending.style == XD_STYLE_LUNA)) {
        d->pending.scheme[0] = '\0';
        ptrdiff_t first = listed_scheme(d, 0);
        if (d->pending.style == XD_STYLE_LUNA && first >= 0)
            snprintf(d->pending.scheme, sizeof(d->pending.scheme), "%s",
                     d->schemes.stems[first]);
        fill_schemes(d);
        xd_widget_redraw(d->scheme_combo);
    }
    xd_widget_redraw(d->appearance_preview);
    changed(d);
}

static void on_scheme(xd_widget *w, void *user) {
    desk *d = user;
    int i = xd_widget_selected(w);
    if (i < 0)
        return;
    ptrdiff_t k = listed_scheme(d, i);
    if (k < 0)
        d->pending.scheme[0] = '\0';
    else
        snprintf(d->pending.scheme, sizeof(d->pending.scheme), "%s", d->schemes.stems[k]);
    xd_widget_redraw(d->appearance_preview);
    changed(d);
}

static void on_font_size(xd_widget *w, void *user) {
    desk *d = user;
    int i = xd_widget_selected(w);
    if (i >= 0 && i <= XD_FONT_EXTRA_LARGE)
        d->pending.font_size = (xd_font_size)i;
    xd_widget_redraw(d->appearance_preview);
    changed(d);
}

static void build_appearance(desk *d) {
    xd_window *win = d->win;
    const int P = PAGE_APPEARANCE;
    d->appearance_preview =
        xd_custom(win, (xd_rect){20, 40, 364, 186}, draw_appearance_preview, d);
    xd_widget_set_page(d->appearance_preview, P);

    xd_widget_set_page(xd_label(win, (xd_rect){20, 238, 200, 16}, "Windows and buttons:", 0), P);
    d->style_combo = xd_combo(win, (xd_rect){20, 256, 220, 21}, false, on_style, d);
    xd_widget_set_page(d->style_combo, P);
    for (int i = 0; i < XD_STYLE_COUNT; i++)
        xd_widget_add_item(d->style_combo, xd_visual_style_name((xd_visual_style)i));
    xd_widget_select(d->style_combo, (int)d->pending.style);

    xd_widget_set_page(xd_label(win, (xd_rect){20, 286, 200, 16}, "Color scheme:", 0), P);
    d->scheme_combo = xd_combo(win, (xd_rect){20, 304, 220, 21}, false, on_scheme, d);
    xd_widget_set_page(d->scheme_combo, P);
    fill_schemes(d);

    xd_widget_set_page(xd_label(win, (xd_rect){20, 334, 200, 16}, "Font size:", 0), P);
    d->font_combo = xd_combo(win, (xd_rect){20, 352, 220, 21}, false, on_font_size, d);
    xd_widget_set_page(d->font_combo, P);
    static const char *const sizes[] = {"Normal", "Large Fonts", "Extra Large Fonts"};
    for (size_t i = 0; i < XD_ARRAY_LEN(sizes); i++)
        xd_widget_add_item(d->font_combo, sizes[i]);
    xd_widget_select(d->font_combo, (int)d->pending.font_size);

    /* Per-item colours/fonts editor, as in Windows 2000, comes later. */
    on_page_disabled(xd_button(win, (xd_rect){298, 352, 86, 23}, "Advanced", nullptr, nullptr), P);
}

/* ---- Effects ------------------------------------------------------------------------ */

static void on_smoothing(xd_widget *w, void *user) {
    (void)w;
    desk *d = user;
    bool on = xd_checkbox_checked(d->smoothing_check);
    xd_widget_set_enabled(d->smoothing_combo, on);
    if (!on)
        d->pending.font_smoothing = XD_SMOOTH_NONE;
    else
        d->pending.font_smoothing = xd_widget_selected(d->smoothing_combo) == 1
                                        ? XD_SMOOTH_CLEARTYPE
                                        : XD_SMOOTH_STANDARD;
    changed(d);
}

static void build_effects(desk *d) {
    xd_window *win = d->win;
    const int P = PAGE_EFFECTS;
    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 40, 364, 190}, "Visual effects"), P);
    on_page_disabled(xd_checkbox(win, (xd_rect){32, 62, 340, 20},
                                 "Use transition effects for menus and tooltips",
                                 d->pending.menu_transitions, nullptr, nullptr),
                     P);
    d->smoothing_check = xd_checkbox(win, (xd_rect){32, 88, 200, 20},
                                     "Smooth edges of screen fonts",
                                     d->pending.font_smoothing != XD_SMOOTH_NONE, on_smoothing, d);
    xd_widget_set_page(d->smoothing_check, P);
    d->smoothing_combo = xd_combo(win, (xd_rect){236, 87, 136, 21}, false, on_smoothing, d);
    xd_widget_set_page(d->smoothing_combo, P);
    xd_widget_add_item(d->smoothing_combo, "Standard");
    xd_widget_add_item(d->smoothing_combo, "ClearType");
    xd_widget_select(d->smoothing_combo, d->pending.font_smoothing == XD_SMOOTH_CLEARTYPE ? 1 : 0);
    xd_widget_set_enabled(d->smoothing_combo, d->pending.font_smoothing != XD_SMOOTH_NONE);

    on_page_disabled(xd_checkbox(win, (xd_rect){32, 114, 340, 20}, "Use large icons",
                                 d->pending.large_icons, nullptr, nullptr),
                     P);
    on_page_disabled(xd_checkbox(win, (xd_rect){32, 140, 340, 20},
                                 "Show window contents while dragging",
                                 d->pending.full_window_drag, nullptr, nullptr),
                     P);
    on_page_disabled(xd_checkbox(win, (xd_rect){32, 166, 340, 34},
                                 "Hide keyboard navigation indicators until I use the Alt key",
                                 d->pending.hide_keyboard_cues, nullptr, nullptr),
                     P);
    xd_widget_set_page(xd_label(win, (xd_rect){20, 238, 364, 16},
                                "Greyed-out effects aren't available yet.", XD_LABEL_DIM),
                       P);
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
static void load_heads(desk *d) {
    free_heads(d->heads, d->head_count);
    d->heads = nullptr;
    d->head_count = 0;
    const xd_display_head *heads = nullptr;
    size_t n = 0;
    if (!xd_ui_display_heads(d->ui, &heads, &n))
        return;
    d->heads = xd_alloc_array(XD_MAX(n, 1), sizeof(*d->heads));
    for (size_t i = 0; i < n; i++) {
        const xd_display_head *h = &heads[i];
        disp_head *dh = &d->heads[i];
        *dh = (disp_head){0};
        snprintf(dh->set.name, sizeof(dh->set.name), "%s", h->name);
        snprintf(dh->description, sizeof(dh->description), "%s", h->description);
        dh->set.enabled = h->enabled;
        dh->set.x = h->x;
        dh->set.y = h->y;
        dh->set.scale = h->scale > 0 ? h->scale : 1.0;
        dh->set.transform = h->transform;
        dh->mode_count = h->mode_count;
        dh->modes = xd_alloc_array(XD_MAX(h->mode_count, 1), sizeof(*dh->modes));
        memcpy(dh->modes, h->modes, h->mode_count * sizeof(*h->modes));
        dh->set.mode = h->mode;
        if (dh->set.mode < 0) {
            /* Off (or a custom mode): offer the preferred one. */
            dh->set.mode = 0;
            for (size_t m = 0; m < h->mode_count; m++) {
                if (h->modes[m].preferred)
                    dh->set.mode = (int)m;
            }
        }
    }
    d->head_count = n;
}

static disp_head *sel_head(desk *d) {
    if (d->sel < 0 || (size_t)d->sel >= d->head_count)
        return nullptr;
    return &d->heads[d->sel];
}

static const xd_display_mode *head_mode(const disp_head *h) {
    if (h->set.mode < 0 || (size_t)h->set.mode >= h->mode_count)
        return nullptr;
    return &h->modes[h->set.mode];
}

/* Size on the desktop, in logical units. */
static void head_size(const disp_head *h, int *w, int *hh) {
    const xd_display_mode *m = head_mode(h);
    int pw = m != nullptr ? m->width : 1024;
    int ph = m != nullptr ? m->height : 768;
    if (h->set.transform == XD_TRANSFORM_90 || h->set.transform == XD_TRANSFORM_270) {
        int tmp = pw;
        pw = ph;
        ph = tmp;
    }
    double s = h->set.scale > 0 ? h->set.scale : 1.0;
    *w = (int)lround(pw / s);
    *hh = (int)lround(ph / s);
}

static bool heads_equal(const disp_head *a, const disp_head *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const xd_display_setting *x = &a[i].set;
        const xd_display_setting *y = &b[i].set;
        if (x->enabled != y->enabled || (x->enabled && (x->mode != y->mode || x->x != y->x ||
                                                        x->y != y->y || x->scale != y->scale ||
                                                        x->transform != y->transform)))
            return false;
    }
    return true;
}

static void display_changed(desk *d) {
    d->display_dirty = d->saved_count != d->head_count ||
                       !heads_equal(d->heads, d->saved_heads, d->head_count);
    xd_widget_redraw(d->monitors);
    changed(d);
}

/* Moves the layout so it starts at (0, 0), as the compositor would. */
static void normalize(desk *d) {
    int minx = INT32_MAX;
    int miny = INT32_MAX;
    for (size_t i = 0; i < d->head_count; i++) {
        if (!d->heads[i].set.enabled)
            continue;
        minx = XD_MIN(minx, d->heads[i].set.x);
        miny = XD_MIN(miny, d->heads[i].set.y);
    }
    if (minx == INT32_MAX)
        return;
    for (size_t i = 0; i < d->head_count; i++) {
        d->heads[i].set.x -= minx;
        d->heads[i].set.y -= miny;
    }
}

/* After a monitor changes size, monitors beyond its right or bottom edge
 * follow it so the desktop stays joined up. */
static void reflow(desk *d, size_t changed_i, int old_w, int old_h) {
    disp_head *c = &d->heads[changed_i];
    int w;
    int h;
    head_size(c, &w, &h);
    int right = c->set.x + old_w;
    int bottom = c->set.y + old_h;
    for (size_t i = 0; i < d->head_count; i++) {
        disp_head *o = &d->heads[i];
        if (i == changed_i || !o->set.enabled)
            continue;
        if (o->set.x >= right)
            o->set.x += w - old_w;
        if (o->set.y >= bottom)
            o->set.y += h - old_h;
    }
    normalize(d);
}

/* Distinct resolutions of a head, smallest first. */
static size_t resolutions(const disp_head *h, int *ws, int *hs, size_t cap) {
    size_t n = 0;
    for (size_t m = 0; m < h->mode_count; m++) {
        bool seen = false;
        for (size_t j = 0; j < n; j++)
            seen = seen || (ws[j] == h->modes[m].width && hs[j] == h->modes[m].height);
        if (!seen && n < cap) {
            ws[n] = h->modes[m].width;
            hs[n] = h->modes[m].height;
            n++;
        }
    }
    for (size_t i = 1; i < n; i++) {
        for (size_t j = i; j > 0 && (long)ws[j] * hs[j] < (long)ws[j - 1] * hs[j - 1]; j--) {
            int tw = ws[j];
            int th = hs[j];
            ws[j] = ws[j - 1];
            hs[j] = hs[j - 1];
            ws[j - 1] = tw;
            hs[j - 1] = th;
        }
    }
    return n;
}

static void show_head(desk *d);

static void set_mode_keeping_layout(desk *d, int mode) {
    disp_head *h = sel_head(d);
    if (h == nullptr || mode < 0 || (size_t)mode >= h->mode_count || mode == h->set.mode)
        return;
    int ow;
    int oh;
    head_size(h, &ow, &oh);
    h->set.mode = mode;
    reflow(d, (size_t)d->sel, ow, oh);
}

static void on_resolution(xd_widget *w, void *user) {
    desk *d = user;
    disp_head *h = sel_head(d);
    if (h == nullptr)
        return;
    int ws[128];
    int hs[128];
    size_t n = resolutions(h, ws, hs, XD_ARRAY_LEN(ws));
    int i = xd_slider_value(w);
    if (i < 0 || (size_t)i >= n)
        return;
    /* That size at the refresh rate closest to the current one. */
    const xd_display_mode *cur = head_mode(h);
    int want = cur != nullptr ? cur->refresh_mhz : 60000;
    int best = -1;
    for (size_t m = 0; m < h->mode_count; m++) {
        if (h->modes[m].width != ws[i] || h->modes[m].height != hs[i])
            continue;
        if (best < 0 ||
            abs(h->modes[m].refresh_mhz - want) < abs(h->modes[best].refresh_mhz - want))
            best = (int)m;
    }
    set_mode_keeping_layout(d, best);
    show_head(d);
    display_changed(d);
}

static void on_refresh(xd_widget *w, void *user) {
    desk *d = user;
    int i = xd_widget_selected(w);
    if (i >= 0 && (size_t)i < d->refresh_count)
        set_mode_keeping_layout(d, d->refresh_modes[i]);
    display_changed(d);
}

static void on_scale(xd_widget *w, void *user) {
    desk *d = user;
    disp_head *h = sel_head(d);
    int i = xd_widget_selected(w);
    if (h == nullptr || i < 0 || (size_t)i >= XD_ARRAY_LEN(scales))
        return;
    int ow;
    int oh;
    head_size(h, &ow, &oh);
    h->set.scale = scales[i];
    reflow(d, (size_t)d->sel, ow, oh);
    display_changed(d);
}

static void on_orientation(xd_widget *w, void *user) {
    desk *d = user;
    disp_head *h = sel_head(d);
    int i = xd_widget_selected(w);
    if (h == nullptr || i < 0 || i > 3)
        return;
    int ow;
    int oh;
    head_size(h, &ow, &oh);
    h->set.transform = (xd_display_transform)i;
    reflow(d, (size_t)d->sel, ow, oh);
    display_changed(d);
}

static size_t enabled_count(const desk *d) {
    size_t n = 0;
    for (size_t i = 0; i < d->head_count; i++)
        n += d->heads[i].set.enabled ? 1 : 0;
    return n;
}

static void on_extend(xd_widget *w, void *user) {
    desk *d = user;
    disp_head *h = sel_head(d);
    if (h == nullptr)
        return;
    bool on = xd_checkbox_checked(w);
    if (!on && enabled_count(d) <= 1) {
        xd_checkbox_set(w, true); /* the last monitor stays on */
        return;
    }
    if (on && !h->set.enabled) {
        /* Joins the desktop on the right. */
        int right = 0;
        for (size_t i = 0; i < d->head_count; i++) {
            if (!d->heads[i].set.enabled)
                continue;
            int hw;
            int hh;
            head_size(&d->heads[i], &hw, &hh);
            right = XD_MAX(right, d->heads[i].set.x + hw);
        }
        h->set.x = right;
        h->set.y = 0;
    }
    h->set.enabled = on;
    normalize(d);
    show_head(d);
    display_changed(d);
}

/* Fills the controls from the selected monitor. */
static void show_head(desk *d) {
    disp_head *h = sel_head(d);
    bool have = h != nullptr;
    xd_widget *controls[] = {d->res_slider, d->scale_combo, d->refresh_combo, d->orient_combo,
                             d->extend_check};
    for (size_t i = 0; i < XD_ARRAY_LEN(controls); i++)
        xd_widget_set_enabled(controls[i], have && (h->set.enabled || controls[i] == d->extend_check));
    if (!have) {
        xd_widget_set_text(d->res_label, "");
        return;
    }
    int ws[128];
    int hs[128];
    size_t n = resolutions(h, ws, hs, XD_ARRAY_LEN(ws));
    const xd_display_mode *cur = head_mode(h);
    int idx = 0;
    for (size_t i = 0; i < n; i++) {
        if (cur != nullptr && ws[i] == cur->width && hs[i] == cur->height)
            idx = (int)i;
    }
    xd_slider_set(d->res_slider, 0, XD_MAX((int)n - 1, 0), idx);
    char buf[128];
    if (cur != nullptr)
        snprintf(buf, sizeof(buf), "%d by %d pixels", cur->width, cur->height);
    else
        snprintf(buf, sizeof(buf), "Unknown");
    xd_widget_set_text(d->res_label, buf);

    /* Refresh rates of this resolution, fastest first. */
    xd_widget_clear_items(d->refresh_combo);
    d->refresh_count = 0;
    for (size_t m = 0; m < h->mode_count && d->refresh_count < XD_ARRAY_LEN(d->refresh_modes);
         m++) {
        if (cur == nullptr || h->modes[m].width != cur->width ||
            h->modes[m].height != cur->height)
            continue;
        size_t at = d->refresh_count++;
        while (at > 0 &&
               h->modes[d->refresh_modes[at - 1]].refresh_mhz < h->modes[m].refresh_mhz) {
            d->refresh_modes[at] = d->refresh_modes[at - 1];
            at--;
        }
        d->refresh_modes[at] = (int)m;
    }
    int rsel = 0;
    for (size_t i = 0; i < d->refresh_count; i++) {
        int mhz = h->modes[d->refresh_modes[i]].refresh_mhz;
        if (mhz > 0)
            snprintf(buf, sizeof(buf), "%.3g Hertz", mhz / 1000.0);
        else
            snprintf(buf, sizeof(buf), "Default");
        xd_widget_add_item(d->refresh_combo, buf);
        if (d->refresh_modes[i] == h->set.mode)
            rsel = (int)i;
    }
    if (d->refresh_count == 0)
        xd_widget_add_item(d->refresh_combo, "Default");
    xd_widget_select(d->refresh_combo, rsel);

    int ssel = 0;
    for (size_t i = 0; i < XD_ARRAY_LEN(scales); i++) {
        if (fabs(scales[i] - h->set.scale) < fabs(scales[ssel] - h->set.scale))
            ssel = (int)i;
    }
    xd_widget_select(d->scale_combo, ssel);
    xd_widget_select(d->orient_combo, (int)h->set.transform);
    xd_checkbox_set(d->extend_check, h->set.enabled);
    xd_widget_set_enabled(d->extend_check, !h->set.enabled || enabled_count(d) > 1);
}

/* The monitor map: desktop layout fitted into the widget (logical units). */
typedef struct map_geom {
    double s, ox, oy;
    int minx, miny;
} map_geom;

static xd_rect head_map_rect(const desk *d, size_t i, const map_geom *g, int *disabled_x) {
    const disp_head *h = &d->heads[i];
    int w;
    int hh;
    head_size(h, &w, &hh);
    int x = h->set.x;
    int y = h->set.y;
    if (!h->set.enabled) {
        /* Off monitors sit beside the desktop, greyed. */
        x = *disabled_x;
        y = 0;
        *disabled_x += w + 200;
    }
    return (xd_rect){(int)lround(g->ox + (x - g->minx) * g->s),
                     (int)lround(g->oy + (y - g->miny) * g->s), (int)lround(w * g->s),
                     (int)lround(hh * g->s)};
}

static map_geom map_geometry(const desk *d, int width, int height, int *first_disabled_x) {
    map_geom g = {.s = 1};
    int minx = INT32_MAX;
    int miny = INT32_MAX;
    int maxx = INT32_MIN;
    int maxy = INT32_MIN;
    for (size_t i = 0; i < d->head_count; i++) {
        const disp_head *h = &d->heads[i];
        if (!h->set.enabled)
            continue;
        int w;
        int hh;
        head_size(h, &w, &hh);
        minx = XD_MIN(minx, h->set.x);
        miny = XD_MIN(miny, h->set.y);
        maxx = XD_MAX(maxx, h->set.x + w);
        maxy = XD_MAX(maxy, h->set.y + hh);
    }
    if (minx == INT32_MAX) {
        minx = 0;
        miny = 0;
        maxx = 1024;
        maxy = 768;
    }
    *first_disabled_x = maxx + 200;
    int extra = maxx;
    for (size_t i = 0; i < d->head_count; i++) {
        if (d->heads[i].set.enabled)
            continue;
        int w;
        int hh;
        head_size(&d->heads[i], &w, &hh);
        extra += 200 + w;
        maxy = XD_MAX(maxy, miny + hh);
    }
    maxx = extra;
    double span_w = XD_MAX(maxx - minx, 1);
    double span_h = XD_MAX(maxy - miny, 1);
    g.s = fmin((width - 20) / span_w, (height - 20) / span_h);
    g.ox = (width - span_w * g.s) / 2;
    g.oy = (height - span_h * g.s) / 2;
    g.minx = minx;
    g.miny = miny;
    return g;
}

static void draw_monitors(xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t, void *user) {
    (void)w;
    desk *d = user;
    xd_fill_rect(cr, r, xd_theme_tab_page_color(t));
    if (d->head_count == 0)
        return;
    int disabled_x = 0;
    map_geom g = map_geometry(d, d->monitors_rect.w, d->monitors_rect.h, &disabled_x);
    xd_font_spec big = t->scheme.fonts[XD_FONT_CAPTION];
    big.size_pt *= 2.2;
    double s = t->scale;
    for (size_t pass = 0; pass < 2; pass++) {
        int dx = disabled_x;
        for (size_t i = 0; i < d->head_count; i++) {
            /* The selected monitor last, on top while dragged. */
            bool selected = (int)i == d->sel;
            xd_rect m = head_map_rect(d, i, &g, &dx);
            if (selected != (pass == 1))
                continue;
            if (selected && d->dragging) {
                m.x += (int)lround(d->drag_dx);
                m.y += (int)lround(d->drag_dy);
            }
            xd_rect dm = {r.x + (int)lround(m.x * s), r.y + (int)lround(m.y * s),
                          (int)lround(m.w * s), (int)lround(m.h * s)};
            bool on = d->heads[i].set.enabled;
            xd_fill_rect(cr, dm, on ? xd_rgba_hex(0x3A6EA5) : xd_rgba_hex(0xA0A0A0));
            xd_draw_bevel(cr, dm, xd_tpx(t, selected ? 2 : 1),
                          selected ? xd_rgba_hex(0x000000) : xd_rgba_hex(0xFFFFFF),
                          xd_rgba_hex(0x000000));
            char num[24];
            snprintf(num, sizeof(num), "%zu", i + 1);
            xd_text_style st = xd_text(t, XD_FONT_CAPTION,
                                       on ? xd_rgba_hex(0xFFFFFF) : xd_rgba_hex(0x606060),
                                       XD_ALIGN_CENTER);
            st.font = &big;
            xd_draw_text(cr, dm, num, &st);
        }
    }
}

/* Places a dragged monitor against the nearest edge of another, as
 * Windows does: monitors always touch. */
static void snap(desk *d, size_t moved, int x, int y) {
    disp_head *m = &d->heads[moved];
    int mw;
    int mh;
    head_size(m, &mw, &mh);
    long best = LONG_MAX;
    int bx = x;
    int by = y;
    for (size_t i = 0; i < d->head_count; i++) {
        const disp_head *o = &d->heads[i];
        if (i == moved || !o->set.enabled)
            continue;
        int ow;
        int oh;
        head_size(o, &ow, &oh);
        int cy = XD_CLAMP(y, o->set.y - mh + 1, o->set.y + oh - 1);
        int cx = XD_CLAMP(x, o->set.x - mw + 1, o->set.x + ow - 1);
        /* Line edges up when they're close. */
        if (abs(cy - o->set.y) < 64)
            cy = o->set.y;
        if (abs(cx - o->set.x) < 64)
            cx = o->set.x;
        int cand[4][2] = {{o->set.x + ow, cy}, {o->set.x - mw, cy}, {cx, o->set.y + oh},
                          {cx, o->set.y - mh}};
        for (size_t c = 0; c < 4; c++) {
            long dx = cand[c][0] - x;
            long dy = cand[c][1] - y;
            long dist = dx * dx + dy * dy;
            if (dist < best) {
                best = dist;
                bx = cand[c][0];
                by = cand[c][1];
            }
        }
    }
    m->set.x = bx;
    m->set.y = by;
    normalize(d);
}

static void on_monitors_pointer([[maybe_unused]] xd_widget *w, xd_pointer_event ev, double x,
                                double y, void *user) {
    desk *d = user;
    int disabled_x = 0;
    map_geom g = map_geometry(d, d->monitors_rect.w, d->monitors_rect.h, &disabled_x);
    if (ev == XD_POINTER_PRESS) {
        int dx = disabled_x;
        for (size_t i = 0; i < d->head_count; i++) {
            xd_rect m = head_map_rect(d, i, &g, &dx);
            if (x >= m.x && y >= m.y && x < m.x + m.w && y < m.y + m.h) {
                d->sel = (int)i;
                xd_widget_select(d->display_combo, d->sel);
                show_head(d);
                d->dragging = d->heads[i].set.enabled && enabled_count(d) > 1;
                d->drag_x0 = x;
                d->drag_y0 = y;
                d->drag_dx = d->drag_dy = 0;
            }
        }
        xd_widget_redraw(d->monitors);
        return;
    }
    if (!d->dragging)
        return;
    d->drag_dx = x - d->drag_x0;
    d->drag_dy = y - d->drag_y0;
    if (ev == XD_POINTER_RELEASE) {
        d->dragging = false;
        disp_head *h = sel_head(d);
        if (h != nullptr && (fabs(d->drag_dx) > 2 || fabs(d->drag_dy) > 2)) {
            int nx = h->set.x + (int)lround(d->drag_dx / g.s);
            int ny = h->set.y + (int)lround(d->drag_dy / g.s);
            snap(d, (size_t)d->sel, nx, ny);
            display_changed(d);
        }
    }
    xd_widget_redraw(d->monitors);
}

static void on_display(xd_widget *w, void *user) {
    desk *d = user;
    d->sel = xd_widget_selected(w);
    show_head(d);
    xd_widget_redraw(d->monitors);
}

static void fill_display_combo(desk *d) {
    xd_widget_clear_items(d->display_combo);
    for (size_t i = 0; i < d->head_count; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%zu. %s on %s", i + 1,
                 d->heads[i].description[0] != '\0' ? d->heads[i].description : "Monitor",
                 d->heads[i].set.name);
        xd_widget_add_item(d->display_combo, buf);
    }
    if (d->sel < 0 || (size_t)d->sel >= d->head_count)
        d->sel = d->head_count > 0 ? 0 : -1;
    xd_widget_select(d->display_combo, d->sel);
}

/* The compositor reported a new configuration (ours applied, a monitor
 * plugged in, or another program's change). */
static void on_display_change(void *user) {
    desk *d = user;
    if (d->display_dirty || d->confirm != nullptr)
        return; /* keep the user's unapplied changes */
    load_heads(d);
    if (d->saved_heads == nullptr || d->saved_count != d->head_count) {
        free_heads(d->saved_heads, d->saved_count);
        d->saved_heads = copy_heads(d->heads, d->head_count);
        d->saved_count = d->head_count;
    }
    fill_display_combo(d);
    show_head(d);
    xd_widget_redraw(d->monitors);
}

static void build_settings(desk *d) {
    xd_window *win = d->win;
    const int P = PAGE_SETTINGS;
    xd_widget_set_page(xd_label(win, (xd_rect){20, 40, 364, 16},
                                "Drag the monitor icons to match the physical arrangement of your "
                                "monitors.",
                                XD_LABEL_WRAP),
                       P);
    d->monitors_rect = (xd_rect){20, 70, 364, 112};
    d->monitors = xd_custom(win, d->monitors_rect, draw_monitors, d);
    xd_custom_set_pointer(d->monitors, on_monitors_pointer);
    xd_widget_set_page(d->monitors, P);
    xd_widget_set_page(xd_label(win, (xd_rect){20, 192, 50, 16}, "Display:", 0), P);
    d->display_combo = xd_combo(win, (xd_rect){72, 189, 312, 21}, false, on_display, d);
    xd_widget_set_page(d->display_combo, P);

    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 218, 176, 82}, "Screen area"), P);
    xd_widget_set_page(xd_label(win, (xd_rect){30, 240, 30, 16}, "Less", 0), P);
    xd_widget_set_page(xd_label(win, (xd_rect){156, 240, 32, 16}, "More", 0), P);
    d->res_slider = xd_slider(win, (xd_rect){58, 236, 98, 30}, 0, 0, 0, on_resolution, d);
    xd_widget_set_page(d->res_slider, P);
    d->res_label = xd_label(win, (xd_rect){30, 274, 158, 16}, "", 0);
    xd_widget_set_page(d->res_label, P);

    xd_widget_set_page(xd_groupbox(win, (xd_rect){206, 218, 178, 82}, "Scale"), P);
    d->scale_combo = xd_combo(win, (xd_rect){218, 240, 154, 21}, false, on_scale, d);
    xd_widget_set_page(d->scale_combo, P);
    for (size_t i = 0; i < XD_ARRAY_LEN(scales); i++) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d%%%s", (int)lround(scales[i] * 100),
                 i == 0 ? " (normal size)" : "");
        xd_widget_add_item(d->scale_combo, buf);
    }
    xd_widget_set_page(
        xd_label(win, (xd_rect){218, 266, 160, 30}, "Makes text and windows larger.",
                 XD_LABEL_WRAP | XD_LABEL_DIM),
        P);

    xd_widget_set_page(xd_label(win, (xd_rect){20, 312, 80, 16}, "Refresh rate:", 0), P);
    d->refresh_combo = xd_combo(win, (xd_rect){104, 309, 92, 21}, false, on_refresh, d);
    xd_widget_set_page(d->refresh_combo, P);
    xd_widget_set_page(xd_label(win, (xd_rect){206, 312, 72, 16}, "Orientation:", 0), P);
    d->orient_combo = xd_combo(win, (xd_rect){280, 309, 104, 21}, false, on_orientation, d);
    xd_widget_set_page(d->orient_combo, P);
    for (size_t i = 0; i < XD_ARRAY_LEN(orientations); i++)
        xd_widget_add_item(d->orient_combo, orientations[i]);

    d->extend_check = xd_checkbox(win, (xd_rect){20, 340, 300, 20},
                                  "Extend my desktop onto this monitor.", true, on_extend, d);
    xd_widget_set_page(d->extend_check, P);
    d->display_note = xd_label(win, (xd_rect){20, 368, 364, 30}, "", XD_LABEL_WRAP | XD_LABEL_DIM);
    xd_widget_set_page(d->display_note, P);

    xd_ui_on_display_change(d->ui, on_display_change, d);
    d->sel = 0;
    on_display_change(d);
    if (d->head_count == 0) {
        xd_widget_set_text(d->display_note,
                           "This compositor doesn't let programs change display settings.");
        xd_widget_set_enabled(d->display_combo, false);
    }
}

/* ---- Monitor Settings confirmation -------------------------------------------- */

static void apply_display(desk *d, const disp_head *heads, size_t count, bool confirm);

static void confirm_close(desk *d) {
    if (d->confirm_timer != nullptr) {
        xd_ui_remove_source(d->confirm_timer);
        d->confirm_timer = nullptr;
    }
    if (d->confirm != nullptr) {
        xd_window *c = d->confirm;
        d->confirm = nullptr;
        xd_window_close(c);
    }
}

static void confirm_keep([[maybe_unused]] xd_widget *w, void *user) {
    desk *d = user;
    free_heads(d->saved_heads, d->saved_count);
    d->saved_heads = copy_heads(d->heads, d->head_count);
    d->saved_count = d->head_count;
    d->display_dirty = false;
    confirm_close(d);
    changed(d);
    if (d->close_after_apply)
        xd_window_close(d->win);
}

static void confirm_revert([[maybe_unused]] xd_widget *w, void *user) {
    desk *d = user;
    confirm_close(d);
    d->close_after_apply = false;
    /* Back to how it was; the change event then reloads the controls. */
    free_heads(d->heads, d->head_count);
    d->heads = copy_heads(d->saved_heads, d->saved_count);
    d->head_count = d->saved_count;
    d->display_dirty = false;
    apply_display(d, d->saved_heads, d->saved_count, false);
    show_head(d);
    changed(d);
}

static void confirm_tick(void *user) {
    desk *d = user;
    if (--d->confirm_left <= 0) {
        confirm_revert(nullptr, d); /* also removes this timer */
        return;
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "Reverting to the previous settings in %d second%s.",
             d->confirm_left, d->confirm_left == 1 ? "" : "s");
    xd_widget_set_text(d->confirm_label, buf);
}

static void confirm_open(desk *d) {
    d->confirm = xd_window_create(d->ui, "Monitor Settings", "xd-desk", 330, 124);
    xd_image(d->confirm, (xd_rect){14, 14, 32, 32}, "dialog-question");
    xd_label(d->confirm, (xd_rect){58, 14, 260, 32},
             "Your desktop has been reconfigured. Do you want to keep these settings?",
             XD_LABEL_WRAP);
    d->confirm_left = 15;
    d->confirm_label = xd_label(d->confirm, (xd_rect){58, 52, 260, 16},
                                "Reverting to the previous settings in 15 seconds.", XD_LABEL_DIM);
    xd_widget *yes = xd_button(d->confirm, (xd_rect){84, 86, 75, 23}, "Yes", confirm_keep, d);
    xd_widget *no = xd_button(d->confirm, (xd_rect){171, 86, 75, 23}, "No", confirm_revert, d);
    xd_window_set_default(d->confirm, yes);
    xd_window_set_cancel(d->confirm, no);
    xd_window_set_focus(d->confirm, yes);
    xd_window_show(d->confirm);
    /* Removed by confirm_close (or the last tick). */
    d->confirm_timer = xd_ui_add_timer(d->ui, 1000, true, confirm_tick, d);
}

typedef struct apply_ctx {
    desk *d;
    bool confirm;
} apply_ctx;

static void on_display_applied(bool ok, void *user) {
    apply_ctx *ctx = user;
    desk *d = ctx->d;
    bool confirm = ctx->confirm;
    xd_free(ctx);
    if (!ok) {
        xd_widget_set_text(d->display_note,
                           "The display settings couldn't be applied. The monitor may not "
                           "support them.");
        d->close_after_apply = false;
        return;
    }
    xd_widget_set_text(d->display_note, "");
    if (confirm)
        confirm_open(d);
}

static void apply_display(desk *d, const disp_head *heads, size_t count, bool confirm) {
    xd_display_setting *sets = xd_alloc_array(XD_MAX(count, 1), sizeof(*sets));
    for (size_t i = 0; i < count; i++)
        sets[i] = heads[i].set;
    apply_ctx *ctx = XD_NEW(apply_ctx);
    ctx->d = d;
    ctx->confirm = confirm;
    xd_ui_display_apply(d->ui, sets, count, on_display_applied, ctx);
    xd_free(sets);
}

/* ---- buttons ------------------------------------------------------------------- */

static bool apply(desk *d) {
    if (d->display_dirty && d->confirm == nullptr)
        apply_display(d, d->heads, d->head_count, true);
    if (xd_settings_equal(&d->saved, &d->pending))
        return true;
    /* Only what changed here: another program may have saved since. */
    xd_settings now;
    xd_settings_load(&now);
    xd_settings_merge(&d->saved, &d->pending, &now);
    if (!xd_settings_save(&now)) {
        fprintf(stderr, "xd-desk: cannot write settings\n");
        return false;
    }
    d->saved = d->pending = now;
    /* Re-theme this dialog too, as Windows does after Apply. */
    xd_scheme scheme;
    xd_settings_scheme(&d->pending, &scheme);
    xd_ui_set_theme(d->ui, d->pending.style, &scheme);
    changed(d);
    return true;
}

static void on_ok([[maybe_unused]] xd_widget *w, void *user) {
    desk *d = user;
    if (d->confirm != nullptr)
        return; /* answer Monitor Settings first */
    if (d->display_dirty) {
        /* Closes once the new display settings are kept. */
        d->close_after_apply = true;
        apply(d);
        return;
    }
    if (apply(d))
        xd_window_close(d->win);
}

static void on_cancel([[maybe_unused]] xd_widget *w, void *user) {
    desk *d = user;
    xd_window_close(d->win);
}

static void on_apply([[maybe_unused]] xd_widget *w, void *user) { apply(user); }

int main(int argc, char **argv) {
    static desk d;
    signal(SIGCHLD, SIG_IGN); /* Preview and Browse children */
    d.chooser_fd = -1;
    d.ui = xd_ui_connect(argc, argv);
    if (d.ui == nullptr)
        return EXIT_FAILURE;
    xd_settings_load(&d.saved);
    d.pending = d.saved;
    xd_scheme_list_load(&d.schemes);
    load_wallpapers(&d);

    int page = PAGE_APPEARANCE; /* like "desk.cpl,,2" */
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--page") == 0) {
            int64_t v;
            if (xd_str_parse_i64(xd_str_from_cstr(argv[i + 1]), 0, PAGE_SETTINGS, &v))
                page = (int)v;
        }
    }

    d.win = xd_window_create(d.ui, "Display Properties", "xd-desk", 404, 448);
    static const char *const tabs[] = {"Background", "Screen Saver", "Appearance", "Effects",
                                       "Settings"};
    xd_widget *tab = xd_tabs(d.win, (xd_rect){7, 7, 390, 401}, tabs, XD_ARRAY_LEN(tabs),
                             on_page, &d);
    build_background(&d);
    build_screensaver(&d);
    build_appearance(&d);
    build_effects(&d);
    build_settings(&d);
    xd_widget_select(tab, page);
    d.page = page;
    update_animation(&d);

    xd_widget *ok = xd_button(d.win, (xd_rect){160, 416, 75, 23}, "OK", on_ok, &d);
    xd_widget *cancel = xd_button(d.win, (xd_rect){241, 416, 75, 23}, "Cancel", on_cancel, &d);
    d.apply = xd_button(d.win, (xd_rect){322, 416, 75, 23}, "Apply", on_apply, &d);
    xd_window_set_default(d.win, ok);
    xd_window_set_cancel(d.win, cancel);
    xd_window_set_focus(d.win, tab);
    changed(&d);
    xd_window_show(d.win);

    xd_ui_run(d.ui);
    xd_ui_destroy(d.ui);
    if (d.preview_image != nullptr)
        cairo_surface_destroy(d.preview_image);
    if (d.preview_saver != nullptr)
        xd_saver_destroy(d.preview_saver);
    if (d.chooser_fd >= 0)
        close(d.chooser_fd);
    xd_strbuf_free(&d.chooser_out);
    free_heads(d.heads, d.head_count);
    free_heads(d.saved_heads, d.saved_count);
    for (size_t i = 0; i < d.wallpaper_count; i++)
        xd_free(d.wallpapers[i]);
    xd_free(d.wallpapers);
    xd_scheme_list_free(&d.schemes);
    return EXIT_SUCCESS;
}
