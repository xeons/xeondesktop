/* SPDX-License-Identifier: MIT
 * Renders a preview of a theme to PNG: an inactive and an active window,
 * dialog controls, a window of the other controls, and a taskbar. Used to check styles and DPI scaling
 * without a running session.
 *
 *   xd-themeview [--style classic|luna|whistler] [--scheme FILE]
 *                [--scale N] [--out FILE.png]
 *                [--element active-frame|inactive-frame|taskbar [--background RRGGBB]]
 *                [--package DIR|FILE.xdtheme [--variant ID]]
 *                [--filter nearest|integer|smooth|scale2x|...] [--integer 0|1]
 *
 * --package draws with a theme package (docs/THEMES.md) over its base
 * style; --filter and --integer choose how its images are scaled.
 *
 * --element draws just one element, at the size it has in the Windows XP
 * reference screenshot tools/luna-measure.py reads, over magenta (or
 * --background: what shows through glass), for
 * pixel comparisons.
 */
#include <stdlib.h>
#include <string.h>

#include "../src/theme/pkg.h"
#include "../src/theme/theme.h"

typedef struct options {
    xd_visual_style style;
    const char *scheme_path;
    double scale;
    const char *out;
    const char *element;
    const char *package;
    const char *variant;
    xd_image_filter filter;
    bool integer;
    uint32_t background; /* behind an --element */
} options;

static void usage(FILE *file) {
    fprintf(file, "usage: xd-themeview [--style classic|luna|whistler] [--scheme FILE]\n"
                  "                    [--scale N] [--out FILE.png]\n");
}

static bool parse_args(int argc, char **argv, options *config) {
    for (int i = 1; i < argc; i++) {
        xd_str arg = xd_str_from_cstr(argv[i]);
        if (xd_str_eq(arg, XD_STR("--help"))) {
            usage(stdout);
            exit(EXIT_SUCCESS);
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n", argv[i]);
            return false;
        }
        xd_str val = xd_str_from_cstr(argv[++i]);
        if (xd_str_eq(arg, XD_STR("--style"))) {
            if (!xd_visual_style_from_name(val, &config->style)) {
                fprintf(stderr, "unknown style: %s\n", val.ptr);
                return false;
            }
        } else if (xd_str_eq(arg, XD_STR("--scheme"))) {
            config->scheme_path = val.ptr;
        } else if (xd_str_eq(arg, XD_STR("--scale"))) {
            if (!xd_str_parse_double(val, &config->scale) || config->scale < 0.5 ||
                config->scale > 8) {
                fprintf(stderr, "scale must be between 0.5 and 8\n");
                return false;
            }
        } else if (xd_str_eq(arg, XD_STR("--element"))) {
            config->element = val.ptr;
        } else if (xd_str_eq(arg, XD_STR("--background"))) {
            char *end = nullptr;
            unsigned long rgb = strtoul(val.ptr, &end, 16);
            if (end == val.ptr || *end != '\0' || rgb > 0xFFFFFF) {
                fprintf(stderr, "background must be RRGGBB\n");
                return false;
            }
            config->background = (uint32_t)rgb;
        } else if (xd_str_eq(arg, XD_STR("--out"))) {
            config->out = val.ptr;
        } else if (xd_str_eq(arg, XD_STR("--package"))) {
            config->package = val.ptr;
        } else if (xd_str_eq(arg, XD_STR("--variant"))) {
            config->variant = val.ptr;
        } else if (xd_str_eq(arg, XD_STR("--filter"))) {
            if (!xd_image_filter_from_name(val.ptr, &config->filter)) {
                fprintf(stderr, "unknown filter: %s\n", val.ptr);
                return false;
            }
        } else if (xd_str_eq(arg, XD_STR("--integer"))) {
            config->integer = strcmp(val.ptr, "0") != 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", arg.ptr);
            return false;
        }
    }
    return true;
}

static void draw_window(const xd_theme *theme, cairo_t *cairo, xd_rect rect, const xd_frame *frame,
                        bool controls) {
    xd_theme_draw_frame(theme, cairo, rect, frame);
    xd_insets in = xd_theme_frame_insets(theme, frame);
    xd_rect client = xd_rect_inset4(rect, in.left, in.top, in.right, in.bottom);
    xd_fill_rect(cairo, client, xd_theme_dialog_color(theme));
    if (!controls)
        return;

    int x = client.x + xd_tpx(theme, 12);
    int y = client.y + xd_tpx(theme, 12);
    int row = xd_tpx(theme, 22);
    int cw = client.w - xd_tpx(theme, 24);
    xd_theme_draw_checkbox(theme, cairo, (xd_rect){x, y, cw, row}, XD_STATE_CHECKED,
                           "Show window contents while dragging");
    xd_theme_draw_checkbox(theme, cairo, (xd_rect){x, y + row, cw, row}, XD_STATE_FOCUSED,
                           "Use large icons");
    xd_theme_draw_checkbox(theme, cairo, (xd_rect){x, y + 2 * row, cw, row},
                           XD_STATE_CHECKED | XD_STATE_DISABLED, "Smooth edges of screen fonts");

    int bw = xd_tpx(theme, 75);
    int bh = xd_tpx(theme, 23);
    int gap = xd_tpx(theme, 6);
    int by = client.y + client.h - bh - xd_tpx(theme, 10);
    int bx = client.x + client.w - xd_tpx(theme, 10) - 3 * bw - 2 * gap;
    xd_theme_draw_button(theme, cairo, (xd_rect){bx, by, bw, bh}, XD_STATE_DEFAULT, "OK");
    xd_theme_draw_button(theme, cairo, (xd_rect){bx + bw + gap, by, bw, bh}, XD_STATE_HOT,
                         "Cancel");
    xd_theme_draw_button(theme, cairo, (xd_rect){bx + 2 * (bw + gap), by, bw, bh},
                         XD_STATE_DISABLED, "Apply");
}

/* A window of the other controls: a menu bar with a menu open, a
 * toolbar, column headers, a tree, a progress bar, a slider, spin
 * buttons, scroll bars, a status bar and a tooltip. */
static void draw_controls(const xd_theme *theme, cairo_t *cairo, xd_rect window_rect) {
    xd_frame frame = {.title = "Controls",
                      .active = true,
                      .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE};
    xd_theme_draw_frame(theme, cairo, window_rect, &frame);
    xd_insets in = xd_theme_frame_insets(theme, &frame);
    xd_rect client_rect = xd_rect_inset4(window_rect, in.left, in.top, in.right, in.bottom);
    xd_fill_rect(cairo, client_rect, xd_theme_dialog_color(theme));
    int unit = xd_tpx(theme, 1);

    /* Menu bar, with Edit open. */
    int mh = xd_tpx(theme, 20);
    xd_theme_draw_menubar(theme, cairo, (xd_rect){client_rect.x, client_rect.y, client_rect.w, mh},
                          true);
    static const char *const bar[] = {"File", "Edit", "View", "Help"};
    for (int i = 0; i < 4; i++)
        xd_theme_draw_menubar_item(
            theme, cairo, (xd_rect){client_rect.x + i * 44 * unit, client_rect.y, 40 * unit, mh},
            i == 1   ? XD_STATE_PRESSED
            : i == 2 ? XD_STATE_HOT
                     : 0,
            bar[i]);

    /* Toolbar. */
    int y = client_rect.y + mh;
    int th = xd_tpx(theme, 26);
    xd_theme_draw_rebar(theme, cairo, (xd_rect){client_rect.x, y, client_rect.w, th}, true);
    static const char *const tools[] = {"Back", "Up", "Search", "Views"};
    static const uint8_t tstates[] = {0, XD_STATE_HOT, XD_STATE_PRESSED, XD_STATE_CHECKED};
    for (int i = 0; i < 4; i++)
        xd_theme_draw_tool_button(
            theme, cairo,
            (xd_rect){client_rect.x + (12 + i * 54) * unit, y + 2 * unit, 50 * unit, th - 4 * unit},
            tstates[i], tools[i]);
    y += th + 6 * unit;

    /* Column headers and a tree. */
    int hx = client_rect.x + 8 * unit;
    static const char *const cols[] = {"Name", "Size", "Type"};
    for (int i = 0; i < 3; i++)
        xd_theme_draw_header_item(theme, cairo,
                                  (xd_rect){hx + i * 80 * unit, y, 80 * unit, 18 * unit},
                                  i == 1   ? XD_STATE_HOT
                                  : i == 2 ? XD_STATE_PRESSED
                                           : 0,
                                  cols[i]);
    xd_rect list = {hx, y + 18 * unit, 240 * unit, 70 * unit};
    xd_rect inner = xd_theme_draw_field(theme, cairo, list, false);
    int sb = xd_theme_scrollbar_width(theme);
    for (int i = 0; i < 2; i++) {
        xd_rect row = {inner.x + 2 * unit, inner.y + 2 * unit + i * 18 * unit, 16 * unit,
                       16 * unit};
        xd_theme_draw_tree_glyph(theme, cairo, row, i == 1);
        xd_theme_draw_label(theme, cairo,
                            (xd_rect){row.x + 20 * unit, row.y, 120 * unit, 16 * unit},
                            i == 1 ? "Open folder" : "Closed folder", false, false, false);
    }
    xd_theme_draw_vscrollbar(
        theme, cairo, (xd_rect){inner.x + inner.w - sb, inner.y, sb, inner.h - sb}, 2, 4, 12);
    xd_theme_draw_hscrollbar(theme, cairo,
                             (xd_rect){inner.x, inner.y + inner.h - sb, inner.w - sb, sb}, 1, 3, 8);
    xd_theme_draw_sizebox(theme, cairo,
                          (xd_rect){inner.x + inner.w - sb, inner.y + inner.h - sb, sb, sb});

    /* Progress, slider and a spin box, right of the list. */
    int rx = list.x + list.w + 16 * unit, rw = client_rect.x + client_rect.w - rx - 150 * unit;
    xd_theme_draw_progress(theme, cairo, (xd_rect){rx, y, rw, 18 * unit}, 0.45);
    xd_rect channel = {rx + 4 * unit, y + 40 * unit, rw - 8 * unit, 4 * unit};
    xd_theme_draw_trackbar(theme, cairo, channel,
                           (xd_rect){rx + rw / 3, y + 32 * unit, 11 * unit, 21 * unit},
                           XD_STATE_HOT);
    xd_rgba tic = xd_theme_trackbar_tics_color(theme, false);
    for (int i = 0; i <= 10; i++)
        xd_fill_rect(cairo,
                     (xd_rect){channel.x + 5 * unit + i * (channel.w - 10 * unit) / 10,
                               y + 55 * unit, unit, 3 * unit},
                     tic);
    xd_rect field = {rx, y + 66 * unit, 50 * unit, 21 * unit};
    xd_rect fin = xd_theme_draw_field(theme, cairo, field, false);
    int spw = 16 * unit;
    xd_theme_draw_label(theme, cairo, (xd_rect){fin.x + 2 * unit, fin.y, fin.w, fin.h}, "15", false,
                        false, false);
    xd_theme_draw_spin_button(theme, cairo, (xd_rect){field.x + field.w, field.y, spw, field.h / 2},
                              true, XD_STATE_HOT);
    xd_theme_draw_spin_button(
        theme, cairo,
        (xd_rect){field.x + field.w, field.y + field.h / 2, spw, field.h - field.h / 2}, false, 0);

    /* The open menu, over everything. */
    xd_menu_metrics metrics = xd_theme_menu_metrics(theme);
    xd_rect menu = {client_rect.x + client_rect.w - 140 * unit, client_rect.y + mh, 136 * unit,
                    2 * metrics.border + 4 * metrics.item_h + metrics.separator_h};
    xd_theme_draw_menu_frame(theme, cairo, menu);
    int iy = menu.y + metrics.border;
    xd_rect item = {menu.x + metrics.border, iy, menu.w - 2 * metrics.border, metrics.item_h};
    xd_menu_item_extras checked = {.checked = true};
    xd_theme_draw_menu_item(theme, cairo, item, 0, "Cut", false, false);
    item.y += metrics.item_h;
    xd_theme_draw_menu_item(theme, cairo, item, XD_STATE_HOT, "Copy", false, false);
    item.y += metrics.item_h;
    xd_theme_draw_menu_item(theme, cairo, item, XD_STATE_DISABLED, "Paste", false, false);
    item.y += metrics.item_h;
    xd_theme_draw_menu_separator(theme, cairo,
                                 (xd_rect){item.x, item.y, item.w, metrics.separator_h});
    item.y += metrics.separator_h;
    xd_theme_draw_menu_item_ex(theme, cairo, item, 0, "Status Bar", true, false, &checked);

    /* Status bar, and a tooltip over the toolbar. */
    int shh = xd_tpx(theme, 22);
    xd_theme_draw_statusbar(
        theme, cairo,
        (xd_rect){client_rect.x, client_rect.y + client_rect.h - shh, client_rect.w, shh},
        "12 objects", true);
    xd_theme_draw_tooltip(theme, cairo,
                          (xd_rect){client_rect.x + 70 * unit, client_rect.y + mh + th - 2 * unit,
                                    120 * unit, 19 * unit},
                          "Up one level");
}

static void draw_taskbar(const xd_theme *theme, cairo_t *cairo, int width, int height) {
    int th = xd_theme_taskbar_height(theme, theme->style == XD_STYLE_AERO);
    xd_rect bar = {0, height - th, width, th};
    xd_theme_draw_taskbar(theme, cairo, bar, XD_EDGE_BOTTOM);

    bool classic = theme->style == XD_STYLE_CLASSIC;
    int pad = classic ? xd_tpx(theme, 2) : 0;
    int sw = xd_theme_start_button_width(theme, cairo);
    xd_rect start = {bar.x + pad, bar.y + (classic ? xd_tpx(theme, 4) : 0), sw,
                     classic ? bar.h - xd_tpx(theme, 6) : bar.h};
    xd_theme_draw_start_button(theme, cairo, start, XD_STATE_NORMAL);

    int tray_w = xd_tpx(theme, 80);
    xd_rect tray = {bar.x + bar.w - tray_w - pad, start.y, tray_w, start.h};
    xd_theme_draw_tray(theme, cairo, tray, "12:00 PM");

    int task_w = xd_tpx(theme, 160);
    int x = start.x + start.w + xd_tpx(theme, 8);
    int ty = classic ? start.y : bar.y + xd_tpx(theme, 3);
    int tht = classic ? start.h : bar.h - xd_tpx(theme, 5);
    xd_theme_draw_task_button(theme, cairo, (xd_rect){x, ty, task_w, tht}, XD_STATE_CHECKED,
                              "Display Properties", nullptr);
    xd_theme_draw_task_button(theme, cairo,
                              (xd_rect){x + task_w + xd_tpx(theme, 3), ty, task_w, tht},
                              XD_STATE_NORMAL, "My Documents", nullptr);
}

/* One element at its reference geometry. Returns the surface, or nullptr
 * for an unknown name. */
static cairo_surface_t *draw_element(const xd_theme *theme, const char *name, uint32_t background) {
    bool active = strcmp(name, "active-frame") == 0;
    if (active || strcmp(name, "inactive-frame") == 0) {
        int width = xd_tpx(theme, active ? 553 : 800), height = xd_tpx(theme, active ? 323 : 600);
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        cairo_t *cairo = cairo_create(surface);
        xd_fill_rect(cairo, (xd_rect){0, 0, width, height}, xd_rgba_hex(background));
        xd_frame frame = {.title = active ? "Untitled - Notepad" : "My Documents",
                          .active = active,
                          .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE};
        xd_theme_draw_frame(theme, cairo, (xd_rect){0, 0, width, height}, &frame);
        cairo_destroy(cairo);
        return surface;
    }
    if (strcmp(name, "taskbar") == 0) {
        int width = xd_tpx(theme, 1920),
            height = xd_theme_taskbar_height(theme, theme->style == XD_STYLE_AERO);
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        cairo_t *cairo = cairo_create(surface);
        xd_theme_draw_taskbar(theme, cairo, (xd_rect){0, 0, width, height}, XD_EDGE_BOTTOM);
        xd_theme_draw_start_button(
            theme, cairo, (xd_rect){0, 0, xd_theme_start_button_width(theme, cairo), height},
            XD_STATE_NORMAL);
        int ty = xd_tpx(theme, 3), th = xd_tpx(theme, 25), tw = xd_tpx(theme, 160);
        xd_theme_draw_task_button(theme, cairo, (xd_rect){xd_tpx(theme, 106), ty, tw, th},
                                  XD_STATE_NORMAL, "My Documents", nullptr);
        xd_theme_draw_task_button(theme, cairo, (xd_rect){xd_tpx(theme, 266), ty, tw, th},
                                  XD_STATE_CHECKED, "Untitled - Notepad", nullptr);
        xd_theme_draw_tray(theme, cairo,
                           (xd_rect){xd_tpx(theme, 1755), 0, width - xd_tpx(theme, 1755), height},
                           "11:01 PM");
        cairo_destroy(cairo);
        return surface;
    }
    return nullptr;
}

int main(int argc, char **argv) {
    options config = {.style = XD_STYLE_CLASSIC,
                      .scale = 1.0,
                      .out = "themeview.png",
                      .filter = XD_FILTER_SMOOTH,
                      .integer = true,
                      .background = 0xFF00FF};
    if (!parse_args(argc, argv, &config)) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    if (config.scheme_path != nullptr) {
        xd_strbuf err = {0};
        if (!xd_scheme_load(config.scheme_path, &scheme, &err)) {
            fprintf(stderr, "%s: %s\n", config.scheme_path, xd_strbuf_cstr(&err));
            xd_strbuf_free(&err);
            return EXIT_FAILURE;
        }
        xd_strbuf_free(&err);
    }

    xd_pkg *pkg = nullptr;
    if (config.package != nullptr) {
        GError *error = nullptr;
        pkg = xd_pkg_load(config.package, config.variant, XD_SOURCE_ROOT "/data/theme/uxtheme.ini",
                          &error);
        if (pkg == nullptr) {
            fprintf(stderr, "%s\n", error->message);
            g_error_free(error);
            return EXIT_FAILURE;
        }
        xd_theme_use_package(pkg, config.filter, config.integer);
    }

    xd_theme theme;
    xd_theme_init(&theme, config.style, &scheme, config.scale);
    const xd_theme *current_theme = &theme;

    if (config.element != nullptr) {
        cairo_surface_t *element_image =
            draw_element(current_theme, config.element, config.background);
        if (element_image == nullptr) {
            fprintf(stderr, "unknown element: %s\n", config.element);
            return EXIT_FAILURE;
        }
        cairo_status_t st = cairo_surface_write_to_png(element_image, config.out);
        cairo_surface_destroy(element_image);
        return st == CAIRO_STATUS_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    int width = xd_tpx(current_theme, 640);
    int height = xd_tpx(current_theme, 660);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cairo = cairo_create(surface);

    xd_fill_rect(cairo, (xd_rect){0, 0, width, height}, xd_sys(current_theme, XD_COLOR_BACKGROUND));

    xd_frame back = {.title = "My Documents",
                     .active = false,
                     .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE};
    draw_window(current_theme, cairo,
                (xd_rect){xd_tpx(current_theme, 24), xd_tpx(current_theme, 20),
                          xd_tpx(current_theme, 300), xd_tpx(current_theme, 200)},
                &back, false);

    xd_frame front = {.title = "Display Properties",
                      .active = true,
                      .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_HELP};
    draw_window(current_theme, cairo,
                (xd_rect){xd_tpx(current_theme, 190), xd_tpx(current_theme, 90),
                          xd_tpx(current_theme, 400), xd_tpx(current_theme, 220)},
                &front, true);

    draw_controls(current_theme, cairo,
                  (xd_rect){xd_tpx(current_theme, 24), xd_tpx(current_theme, 320),
                            xd_tpx(current_theme, 592), xd_tpx(current_theme, 300)});

    draw_taskbar(current_theme, cairo, width, height);

    cairo_destroy(cairo);
    cairo_status_t st = cairo_surface_write_to_png(surface, config.out);
    cairo_surface_destroy(surface);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "cannot write %s: %s\n", config.out, cairo_status_to_string(st));
        return EXIT_FAILURE;
    }
    printf("wrote %s (%dx%d, %s, %s, scale %.2f%s%s)\n", config.out, width, height,
           xd_visual_style_name(theme.style), scheme.name, config.scale, pkg ? ", package " : "",
           pkg ? xd_pkg_name(pkg) : "");
    xd_theme_use_package(nullptr, XD_FILTER_SMOOTH, true);
    xd_pkg_free(pkg);
    return EXIT_SUCCESS;
}
