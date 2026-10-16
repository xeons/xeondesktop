/* SPDX-License-Identifier: MIT
 * xd-desk: Display Properties.
 *
 * Tabs follow Windows 2000: Background, Screen Saver, Appearance, Effects,
 * Settings. Changes are kept in `pending` and written to desktop.ini on
 * Apply/OK; the compositor and shell pick the file up immediately.
 */
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../core/alloc.h"
#include "../settings/settings.h"
#include "../ui/image.h"
#include "../ui/ui.h"

enum { PAGE_BACKGROUND, PAGE_SCREENSAVER, PAGE_APPEARANCE, PAGE_EFFECTS, PAGE_SETTINGS };

typedef struct desk {
    xd_ui *ui;
    xd_window *win;
    xd_settings saved;
    xd_settings pending;
    xd_widget *apply;

    /* Background */
    char **wallpapers; /* paths; index 0 = (None) = nullptr */
    size_t wallpaper_count, wallpaper_cap;
    xd_widget *wallpaper_list;
    xd_widget *wallpaper_mode;
    xd_widget *bg_preview;
    cairo_surface_t *preview_image;
    int preview_full_w; /* the file's real width, for Center/Tile */
    char preview_path[1024];

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
    xd_widget *display_combo;
    xd_widget *resolution_label;
    xd_widget *scale_label;
    xd_widget *refresh_label;
} desk;

/* ---- helpers --------------------------------------------------------------------- */

static void changed(desk *d) {
    xd_widget_set_enabled(d->apply, !xd_settings_equal(&d->saved, &d->pending));
    xd_widget_redraw(d->apply);
}

static void on_page([[maybe_unused]] xd_widget *w, [[maybe_unused]] void *user) {}

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

    xd_widget *browse = xd_button(win, (xd_rect){298, 236, 86, 23}, "Browse...", nullptr, nullptr);
    xd_widget_set_enabled(browse, false); /* needs a file chooser (GTK3 apps) */
    xd_widget_set_page(browse, PAGE_BACKGROUND);
    xd_widget_set_page(xd_label(win, (xd_rect){298, 330, 86, 16}, "Picture Display:", 0),
                       PAGE_BACKGROUND);
    d->wallpaper_mode = xd_combo(win, (xd_rect){298, 348, 86, 21}, false, on_wallpaper_mode, d);
    xd_widget_set_page(d->wallpaper_mode, PAGE_BACKGROUND);
    static const char *const modes[] = {"Center", "Tile", "Stretch", "Fill", "Fit"};
    for (size_t i = 0; i < XD_ARRAY_LEN(modes); i++)
        xd_widget_add_item(d->wallpaper_mode, modes[i]);
    xd_widget_select(d->wallpaper_mode, (int)d->pending.wallpaper_mode);
}

/* ---- Screen Saver ------------------------------------------------------------------ */

static void draw_saver_preview(xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t,
                               void *user) {
    (void)w;
    desk *d = user;
    xd_rect screen = draw_monitor(cr, r, t, primary_aspect(d));
    xd_fill_rect(cr, screen, xd_rgba_hex(0x000000));
}

static xd_widget *on_page_disabled(xd_widget *w, int page) {
    xd_widget_set_enabled(w, false);
    xd_widget_set_page(w, page);
    return w;
}

static void build_screensaver(desk *d) {
    xd_window *win = d->win;
    const int P = PAGE_SCREENSAVER;
    xd_widget_set_page(xd_custom(win, (xd_rect){105, 42, 194, 166}, draw_saver_preview, d), P);
    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 216, 364, 92}, "Screen Saver"), P);
    xd_widget *saver = on_page_disabled(xd_combo(win, (xd_rect){32, 238, 170, 21}, false,
                                                 nullptr, nullptr),
                                        P);
    xd_widget_add_item(saver, "(None)");
    xd_widget_select(saver, 0);
    on_page_disabled(xd_button(win, (xd_rect){210, 237, 80, 23}, "Settings", nullptr, nullptr), P);
    on_page_disabled(xd_button(win, (xd_rect){296, 237, 80, 23}, "Preview", nullptr, nullptr), P);
    on_page_disabled(xd_checkbox(win, (xd_rect){32, 272, 150, 20}, "Password protected",
                                 d->pending.screensaver_password, nullptr, nullptr),
                     P);
    xd_widget_set_page(xd_label(win, (xd_rect){210, 274, 40, 16}, "Wait:", XD_LABEL_DIM), P);
    xd_widget *wait = on_page_disabled(xd_combo(win, (xd_rect){246, 271, 70, 21}, false, nullptr,
                                                nullptr),
                                       P);
    xd_strbuf minutes = {0};
    xd_strbuf_appendf(&minutes, "%d", d->pending.screensaver_wait);
    xd_widget_add_item(wait, xd_strbuf_cstr(&minutes));
    xd_widget_select(wait, 0);
    xd_strbuf_free(&minutes);
    xd_widget_set_page(xd_label(win, (xd_rect){322, 274, 56, 16}, "minutes", XD_LABEL_DIM), P);

    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 316, 364, 64}, "Energy saving features of monitor"), P);
    xd_widget_set_page(xd_image(win, (xd_rect){32, 338, 32, 32}, "preferences-system-power-management"), P);
    xd_widget_set_page(xd_label(win, (xd_rect){74, 336, 210, 32},
                                "To adjust the power settings for your monitor, click Power.",
                                XD_LABEL_WRAP),
                       P);
    on_page_disabled(xd_button(win, (xd_rect){296, 340, 80, 23}, "Power...", nullptr, nullptr), P);
    xd_widget_set_page(xd_label(win, (xd_rect){20, 388, 364, 16},
                                "Screen savers aren't available yet.", XD_LABEL_DIM),
                       P);
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

static void on_style(xd_widget *w, void *user) {
    desk *d = user;
    int i = xd_widget_selected(w);
    if (i >= 0 && i < XD_STYLE_COUNT)
        d->pending.style = (xd_visual_style)i;
    xd_widget_redraw(d->appearance_preview);
    changed(d);
}

static void on_scheme(xd_widget *w, void *user) {
    desk *d = user;
    int i = xd_widget_selected(w);
    if (i < 0)
        return;
    /* Item 0 is the built-in Windows Standard. */
    if (i == 0)
        d->pending.scheme[0] = '\0';
    else if ((size_t)(i - 1) < d->schemes.count)
        snprintf(d->pending.scheme, sizeof(d->pending.scheme), "%s", d->schemes.stems[i - 1]);
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
    xd_widget_add_item(d->scheme_combo, "Windows Standard (built in)");
    int current = 0;
    for (size_t i = 0; i < d->schemes.count; i++) {
        xd_widget_add_item(d->scheme_combo, d->schemes.names[i]);
        if (strcmp(d->schemes.stems[i], d->pending.scheme) == 0)
            current = (int)i + 1;
    }
    xd_widget_select(d->scheme_combo, current);

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

static void draw_monitors(xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t, void *user) {
    (void)w;
    desk *d = user;
    xd_fill_rect(cr, r, xd_theme_tab_page_color(t));
    const xd_output_info *outs = nullptr;
    size_t n = xd_ui_outputs(d->ui, &outs);
    if (n == 0)
        return;
    /* Fit the whole desktop layout into r, keeping proportions. */
    int minx = outs[0].x;
    int miny = outs[0].y;
    int maxx = outs[0].x + outs[0].logical_w;
    int maxy = outs[0].y + outs[0].logical_h;
    for (size_t i = 1; i < n; i++) {
        minx = XD_MIN(minx, outs[i].x);
        miny = XD_MIN(miny, outs[i].y);
        maxx = XD_MAX(maxx, outs[i].x + outs[i].logical_w);
        maxy = XD_MAX(maxy, outs[i].y + outs[i].logical_h);
    }
    double span_w = XD_MAX(maxx - minx, 1);
    double span_h = XD_MAX(maxy - miny, 1);
    double s = fmin((r.w - xd_tpx(t, 20)) / span_w, (r.h - xd_tpx(t, 20)) / span_h);
    double ox = r.x + (r.w - span_w * s) / 2;
    double oy = r.y + (r.h - span_h * s) / 2;
    int sel = xd_widget_selected(d->display_combo);

    xd_font_spec big = t->scheme.fonts[XD_FONT_CAPTION];
    big.size_pt *= 2.2;
    for (size_t i = 0; i < n; i++) {
        xd_rect m = {(int)(ox + (outs[i].x - minx) * s), (int)(oy + (outs[i].y - miny) * s),
                     (int)(outs[i].logical_w * s), (int)(outs[i].logical_h * s)};
        bool selected = (int)i == sel;
        xd_fill_rect(cr, m, xd_rgba_hex(0x3A6EA5));
        xd_draw_bevel(cr, m, xd_tpx(t, selected ? 2 : 1),
                      selected ? xd_rgba_hex(0x000000) : xd_rgba_hex(0xFFFFFF),
                      xd_rgba_hex(0x000000));
        char num[24];
        snprintf(num, sizeof(num), "%zu", i + 1);
        xd_text_style st = xd_text(t, XD_FONT_CAPTION, xd_rgba_hex(0xFFFFFF), XD_ALIGN_CENTER);
        st.font = &big;
        xd_draw_text(cr, m, num, &st);
    }
}

static void show_output(desk *d) {
    const xd_output_info *outs = nullptr;
    size_t n = xd_ui_outputs(d->ui, &outs);
    int i = xd_widget_selected(d->display_combo);
    if (i < 0 || (size_t)i >= n)
        return;
    const xd_output_info *o = &outs[i];
    xd_strbuf s = {0};
    xd_strbuf_appendf(&s, "%d by %d pixels", o->mode_w, o->mode_h);
    xd_widget_set_text(d->resolution_label, xd_strbuf_cstr(&s));
    xd_strbuf_clear(&s);
    if (o->logical_w > 0)
        xd_strbuf_appendf(&s, "%.0f%% (%d by %d logical)",
                          100.0 * o->mode_w / o->logical_w, o->logical_w, o->logical_h);
    xd_widget_set_text(d->scale_label, xd_strbuf_cstr(&s));
    xd_strbuf_clear(&s);
    if (o->refresh_mhz > 0)
        xd_strbuf_appendf(&s, "%.0f Hertz", o->refresh_mhz / 1000.0);
    else
        xd_strbuf_append(&s, XD_STR("Not reported by this display"));
    xd_widget_set_text(d->refresh_label, xd_strbuf_cstr(&s));
    xd_strbuf_free(&s);
}

static void on_display(xd_widget *w, void *user) {
    desk *d = user;
    show_output(d);
    xd_widget_redraw(w);
}

static void build_settings(desk *d) {
    xd_window *win = d->win;
    const int P = PAGE_SETTINGS;
    xd_widget_set_page(xd_label(win, (xd_rect){20, 40, 364, 16},
                                "Drag the monitor icons to match the physical arrangement.",
                                XD_LABEL_DIM),
                       P);
    xd_widget_set_page(xd_custom(win, (xd_rect){20, 60, 364, 130}, draw_monitors, d), P);
    xd_widget_set_page(xd_label(win, (xd_rect){20, 202, 50, 16}, "Display:", 0), P);
    d->display_combo = xd_combo(win, (xd_rect){72, 199, 312, 21}, false, on_display, d);
    xd_widget_set_page(d->display_combo, P);
    const xd_output_info *outs = nullptr;
    size_t n = xd_ui_outputs(d->ui, &outs);
    for (size_t i = 0; i < n; i++) {
        xd_strbuf s = {0};
        xd_strbuf_appendf(&s, "%zu. %s on %s", i + 1,
                          outs[i].description[0] != '\0' ? outs[i].description : "Monitor",
                          outs[i].name);
        xd_widget_add_item(d->display_combo, xd_strbuf_cstr(&s));
        xd_strbuf_free(&s);
    }
    xd_widget_select(d->display_combo, 0);

    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 230, 176, 60}, "Screen area"), P);
    d->resolution_label = xd_label(win, (xd_rect){32, 254, 156, 16}, "", 0);
    xd_widget_set_page(d->resolution_label, P);
    xd_widget_set_page(xd_groupbox(win, (xd_rect){206, 230, 178, 60}, "Scale"), P);
    d->scale_label = xd_label(win, (xd_rect){218, 254, 160, 16}, "", 0);
    xd_widget_set_page(d->scale_label, P);
    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 298, 364, 44}, "Refresh rate"), P);
    d->refresh_label = xd_label(win, (xd_rect){32, 318, 340, 16}, "", 0);
    xd_widget_set_page(d->refresh_label, P);
    on_page_disabled(xd_checkbox(win, (xd_rect){20, 348, 300, 20},
                                 "Extend my desktop onto this monitor", true, nullptr, nullptr),
                     P);
    on_page_disabled(xd_button(win, (xd_rect){298, 372, 86, 23}, "Advanced...", nullptr, nullptr),
                     P);
    xd_widget_set_page(xd_label(win, (xd_rect){20, 376, 270, 16},
                                "Changing display settings isn't available yet.", XD_LABEL_DIM),
                       P);
    show_output(d);
}

/* ---- buttons ------------------------------------------------------------------- */

static bool apply(desk *d) {
    if (!xd_settings_save(&d->pending)) {
        fprintf(stderr, "xd-desk: cannot write settings\n");
        return false;
    }
    d->saved = d->pending;
    /* Re-theme this dialog too, as Windows does after Apply. */
    xd_scheme scheme;
    xd_settings_scheme(&d->pending, &scheme);
    xd_ui_set_theme(d->ui, d->pending.style, &scheme);
    changed(d);
    return true;
}

static void on_ok([[maybe_unused]] xd_widget *w, void *user) {
    desk *d = user;
    if (xd_settings_equal(&d->saved, &d->pending) || apply(d))
        xd_window_close(d->win);
}

static void on_cancel([[maybe_unused]] xd_widget *w, void *user) {
    desk *d = user;
    xd_window_close(d->win);
}

static void on_apply([[maybe_unused]] xd_widget *w, void *user) { apply(user); }

int main(int argc, char **argv) {
    static desk d;
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
    for (size_t i = 0; i < d.wallpaper_count; i++)
        xd_free(d.wallpapers[i]);
    xd_free(d.wallpapers);
    xd_scheme_list_free(&d.schemes);
    return EXIT_SUCCESS;
}
