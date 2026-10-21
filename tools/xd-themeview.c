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

static void usage(FILE *f) {
    fprintf(f, "usage: xd-themeview [--style classic|luna|whistler] [--scheme FILE]\n"
               "                    [--scale N] [--out FILE.png]\n");
}

static bool parse_args(int argc, char **argv, options *o) {
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
            if (!xd_visual_style_from_name(val, &o->style)) {
                fprintf(stderr, "unknown style: %s\n", val.ptr);
                return false;
            }
        } else if (xd_str_eq(arg, XD_STR("--scheme"))) {
            o->scheme_path = val.ptr;
        } else if (xd_str_eq(arg, XD_STR("--scale"))) {
            if (!xd_str_parse_double(val, &o->scale) || o->scale < 0.5 || o->scale > 8) {
                fprintf(stderr, "scale must be between 0.5 and 8\n");
                return false;
            }
        } else if (xd_str_eq(arg, XD_STR("--element"))) {
            o->element = val.ptr;
        } else if (xd_str_eq(arg, XD_STR("--background"))) {
            char *end = nullptr;
            unsigned long rgb = strtoul(val.ptr, &end, 16);
            if (end == val.ptr || *end != '\0' || rgb > 0xFFFFFF) {
                fprintf(stderr, "background must be RRGGBB\n");
                return false;
            }
            o->background = (uint32_t)rgb;
        } else if (xd_str_eq(arg, XD_STR("--out"))) {
            o->out = val.ptr;
        } else if (xd_str_eq(arg, XD_STR("--package"))) {
            o->package = val.ptr;
        } else if (xd_str_eq(arg, XD_STR("--variant"))) {
            o->variant = val.ptr;
        } else if (xd_str_eq(arg, XD_STR("--filter"))) {
            if (!xd_image_filter_from_name(val.ptr, &o->filter)) {
                fprintf(stderr, "unknown filter: %s\n", val.ptr);
                return false;
            }
        } else if (xd_str_eq(arg, XD_STR("--integer"))) {
            o->integer = strcmp(val.ptr, "0") != 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", arg.ptr);
            return false;
        }
    }
    return true;
}

static void draw_window(const xd_theme *t, cairo_t *cr, xd_rect r, const xd_frame *f,
                        bool controls) {
    xd_theme_draw_frame(t, cr, r, f);
    xd_insets in = xd_theme_frame_insets(t, f);
    xd_rect client = xd_rect_inset4(r, in.left, in.top, in.right, in.bottom);
    xd_fill_rect(cr, client, xd_theme_dialog_color(t));
    if (!controls)
        return;

    int x = client.x + xd_tpx(t, 12);
    int y = client.y + xd_tpx(t, 12);
    int row = xd_tpx(t, 22);
    int cw = client.w - xd_tpx(t, 24);
    xd_theme_draw_checkbox(t, cr, (xd_rect){x, y, cw, row}, XD_STATE_CHECKED,
                           "Show window contents while dragging");
    xd_theme_draw_checkbox(t, cr, (xd_rect){x, y + row, cw, row}, XD_STATE_FOCUSED,
                           "Use large icons");
    xd_theme_draw_checkbox(t, cr, (xd_rect){x, y + 2 * row, cw, row},
                           XD_STATE_CHECKED | XD_STATE_DISABLED, "Smooth edges of screen fonts");

    int bw = xd_tpx(t, 75);
    int bh = xd_tpx(t, 23);
    int gap = xd_tpx(t, 6);
    int by = client.y + client.h - bh - xd_tpx(t, 10);
    int bx = client.x + client.w - xd_tpx(t, 10) - 3 * bw - 2 * gap;
    xd_theme_draw_button(t, cr, (xd_rect){bx, by, bw, bh}, XD_STATE_DEFAULT, "OK");
    xd_theme_draw_button(t, cr, (xd_rect){bx + bw + gap, by, bw, bh}, XD_STATE_HOT, "Cancel");
    xd_theme_draw_button(t, cr, (xd_rect){bx + 2 * (bw + gap), by, bw, bh}, XD_STATE_DISABLED,
                         "Apply");
}

/* A window of the other controls: a menu bar with a menu open, a
 * toolbar, column headers, a tree, a progress bar, a slider, spin
 * buttons, scroll bars, a status bar and a tooltip. */
static void draw_controls(const xd_theme *t, cairo_t *cr, xd_rect r) {
    xd_frame f = {.title = "Controls",
                  .active = true,
                  .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE};
    xd_theme_draw_frame(t, cr, r, &f);
    xd_insets in = xd_theme_frame_insets(t, &f);
    xd_rect c = xd_rect_inset4(r, in.left, in.top, in.right, in.bottom);
    xd_fill_rect(cr, c, xd_theme_dialog_color(t));
    int u = xd_tpx(t, 1);

    /* Menu bar, with Edit open. */
    int mh = xd_tpx(t, 20);
    xd_theme_draw_menubar(t, cr, (xd_rect){c.x, c.y, c.w, mh}, true);
    static const char *const bar[] = {"File", "Edit", "View", "Help"};
    for (int i = 0; i < 4; i++)
        xd_theme_draw_menubar_item(t, cr, (xd_rect){c.x + i * 44 * u, c.y, 40 * u, mh},
                                   i == 1 ? XD_STATE_PRESSED : i == 2 ? XD_STATE_HOT : 0, bar[i]);

    /* Toolbar. */
    int y = c.y + mh;
    int th = xd_tpx(t, 26);
    xd_theme_draw_rebar(t, cr, (xd_rect){c.x, y, c.w, th}, true);
    static const char *const tools[] = {"Back", "Up", "Search", "Views"};
    static const uint8_t tstates[] = {0, XD_STATE_HOT, XD_STATE_PRESSED, XD_STATE_CHECKED};
    for (int i = 0; i < 4; i++)
        xd_theme_draw_tool_button(t, cr,
                                  (xd_rect){c.x + (12 + i * 54) * u, y + 2 * u, 50 * u, th - 4 * u},
                                  tstates[i], tools[i]);
    y += th + 6 * u;

    /* Column headers and a tree. */
    int hx = c.x + 8 * u;
    static const char *const cols[] = {"Name", "Size", "Type"};
    for (int i = 0; i < 3; i++)
        xd_theme_draw_header_item(t, cr, (xd_rect){hx + i * 80 * u, y, 80 * u, 18 * u},
                                  i == 1 ? XD_STATE_HOT : i == 2 ? XD_STATE_PRESSED : 0, cols[i]);
    xd_rect list = {hx, y + 18 * u, 240 * u, 70 * u};
    xd_rect inner = xd_theme_draw_field(t, cr, list, false);
    int sb = xd_theme_scrollbar_width(t);
    for (int i = 0; i < 2; i++) {
        xd_rect row = {inner.x + 2 * u, inner.y + 2 * u + i * 18 * u, 16 * u, 16 * u};
        xd_theme_draw_tree_glyph(t, cr, row, i == 1);
        xd_theme_draw_label(t, cr, (xd_rect){row.x + 20 * u, row.y, 120 * u, 16 * u},
                            i == 1 ? "Open folder" : "Closed folder", false, false, false);
    }
    xd_theme_draw_vscrollbar(t, cr,
                             (xd_rect){inner.x + inner.w - sb, inner.y, sb, inner.h - sb}, 2, 4,
                             12);
    xd_theme_draw_hscrollbar(t, cr,
                             (xd_rect){inner.x, inner.y + inner.h - sb, inner.w - sb, sb}, 1, 3,
                             8);
    xd_theme_draw_sizebox(t, cr, (xd_rect){inner.x + inner.w - sb, inner.y + inner.h - sb, sb, sb});

    /* Progress, slider and a spin box, right of the list. */
    int rx = list.x + list.w + 16 * u, rw = c.x + c.w - rx - 150 * u;
    xd_theme_draw_progress(t, cr, (xd_rect){rx, y, rw, 18 * u}, 0.45);
    xd_rect channel = {rx + 4 * u, y + 40 * u, rw - 8 * u, 4 * u};
    xd_theme_draw_trackbar(t, cr, channel,
                           (xd_rect){rx + rw / 3, y + 32 * u, 11 * u, 21 * u}, XD_STATE_HOT);
    xd_rgba tic = xd_theme_trackbar_tics_color(t, false);
    for (int i = 0; i <= 10; i++)
        xd_fill_rect(cr, (xd_rect){channel.x + 5 * u + i * (channel.w - 10 * u) / 10,
                                   y + 55 * u, u, 3 * u},
                     tic);
    xd_rect field = {rx, y + 66 * u, 50 * u, 21 * u};
    xd_rect fin = xd_theme_draw_field(t, cr, field, false);
    int spw = 16 * u;
    xd_theme_draw_label(t, cr, (xd_rect){fin.x + 2 * u, fin.y, fin.w, fin.h}, "15", false, false,
                        false);
    xd_theme_draw_spin_button(t, cr, (xd_rect){field.x + field.w, field.y, spw, field.h / 2}, true,
                              XD_STATE_HOT);
    xd_theme_draw_spin_button(t, cr,
                              (xd_rect){field.x + field.w, field.y + field.h / 2, spw,
                                        field.h - field.h / 2},
                              false, 0);

    /* The open menu, over everything. */
    xd_menu_metrics m = xd_theme_menu_metrics(t);
    xd_rect menu = {c.x + c.w - 140 * u, c.y + mh, 136 * u, 2 * m.border + 4 * m.item_h +
                                                              m.separator_h};
    xd_theme_draw_menu_frame(t, cr, menu);
    int iy = menu.y + m.border;
    xd_rect item = {menu.x + m.border, iy, menu.w - 2 * m.border, m.item_h};
    xd_menu_item_extras checked = {.checked = true};
    xd_theme_draw_menu_item(t, cr, item, 0, "Cut", false, false);
    item.y += m.item_h;
    xd_theme_draw_menu_item(t, cr, item, XD_STATE_HOT, "Copy", false, false);
    item.y += m.item_h;
    xd_theme_draw_menu_item(t, cr, item, XD_STATE_DISABLED, "Paste", false, false);
    item.y += m.item_h;
    xd_theme_draw_menu_separator(t, cr, (xd_rect){item.x, item.y, item.w, m.separator_h});
    item.y += m.separator_h;
    xd_theme_draw_menu_item_ex(t, cr, item, 0, "Status Bar", true, false, &checked);

    /* Status bar, and a tooltip over the toolbar. */
    int shh = xd_tpx(t, 22);
    xd_theme_draw_statusbar(t, cr, (xd_rect){c.x, c.y + c.h - shh, c.w, shh}, "12 objects", true);
    xd_theme_draw_tooltip(t, cr, (xd_rect){c.x + 70 * u, c.y + mh + th - 2 * u, 120 * u, 19 * u},
                          "Up one level");
}

static void draw_taskbar(const xd_theme *t, cairo_t *cr, int width, int height) {
    int th = xd_theme_taskbar_height(t);
    xd_rect bar = {0, height - th, width, th};
    xd_theme_draw_taskbar(t, cr, bar, XD_EDGE_BOTTOM);

    bool classic = t->style == XD_STYLE_CLASSIC;
    int pad = classic ? xd_tpx(t, 2) : 0;
    int sw = xd_theme_start_button_width(t, cr);
    xd_rect start = {bar.x + pad, bar.y + (classic ? xd_tpx(t, 4) : 0), sw,
                     classic ? bar.h - xd_tpx(t, 6) : bar.h};
    xd_theme_draw_start_button(t, cr, start, XD_STATE_NORMAL);

    int tray_w = xd_tpx(t, 80);
    xd_rect tray = {bar.x + bar.w - tray_w - pad, start.y, tray_w, start.h};
    xd_theme_draw_tray(t, cr, tray, "12:00 PM");

    int task_w = xd_tpx(t, 160);
    int x = start.x + start.w + xd_tpx(t, 8);
    int ty = classic ? start.y : bar.y + xd_tpx(t, 3);
    int tht = classic ? start.h : bar.h - xd_tpx(t, 5);
    xd_theme_draw_task_button(t, cr, (xd_rect){x, ty, task_w, tht}, XD_STATE_CHECKED,
                              "Display Properties", nullptr);
    xd_theme_draw_task_button(t, cr, (xd_rect){x + task_w + xd_tpx(t, 3), ty, task_w, tht},
                              XD_STATE_NORMAL, "My Documents", nullptr);
}

/* One element at its reference geometry. Returns the surface, or nullptr
 * for an unknown name. */
static cairo_surface_t *draw_element(const xd_theme *t, const char *name, uint32_t background) {
    bool active = strcmp(name, "active-frame") == 0;
    if (active || strcmp(name, "inactive-frame") == 0) {
        int w = xd_tpx(t, active ? 553 : 800), h = xd_tpx(t, active ? 323 : 600);
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t *cr = cairo_create(s);
        xd_fill_rect(cr, (xd_rect){0, 0, w, h}, xd_rgba_hex(background));
        xd_frame f = {.title = active ? "Untitled - Notepad" : "My Documents",
                      .active = active,
                      .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE};
        xd_theme_draw_frame(t, cr, (xd_rect){0, 0, w, h}, &f);
        cairo_destroy(cr);
        return s;
    }
    if (strcmp(name, "taskbar") == 0) {
        int w = xd_tpx(t, 1920), h = xd_theme_taskbar_height(t);
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t *cr = cairo_create(s);
        xd_theme_draw_taskbar(t, cr, (xd_rect){0, 0, w, h}, XD_EDGE_BOTTOM);
        xd_theme_draw_start_button(t, cr, (xd_rect){0, 0, xd_theme_start_button_width(t, cr), h},
                                   XD_STATE_NORMAL);
        int ty = xd_tpx(t, 3), th = xd_tpx(t, 25), tw = xd_tpx(t, 160);
        xd_theme_draw_task_button(t, cr, (xd_rect){xd_tpx(t, 106), ty, tw, th}, XD_STATE_NORMAL,
                                  "My Documents", nullptr);
        xd_theme_draw_task_button(t, cr, (xd_rect){xd_tpx(t, 266), ty, tw, th},
                                  XD_STATE_CHECKED, "Untitled - Notepad", nullptr);
        xd_theme_draw_tray(t, cr, (xd_rect){xd_tpx(t, 1755), 0, w - xd_tpx(t, 1755), h},
                           "11:01 PM");
        cairo_destroy(cr);
        return s;
    }
    return nullptr;
}

int main(int argc, char **argv) {
    options o = {.style = XD_STYLE_CLASSIC,
                 .scale = 1.0,
                 .out = "themeview.png",
                 .filter = XD_FILTER_SMOOTH,
                 .integer = true,
                 .background = 0xFF00FF};
    if (!parse_args(argc, argv, &o)) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    if (o.scheme_path != nullptr) {
        xd_strbuf err = {0};
        if (!xd_scheme_load(o.scheme_path, &scheme, &err)) {
            fprintf(stderr, "%s: %s\n", o.scheme_path, xd_strbuf_cstr(&err));
            xd_strbuf_free(&err);
            return EXIT_FAILURE;
        }
        xd_strbuf_free(&err);
    }

    xd_pkg *pkg = nullptr;
    if (o.package != nullptr) {
        GError *error = nullptr;
        pkg = xd_pkg_load(o.package, o.variant, XD_SOURCE_ROOT "/data/theme/uxtheme.ini",
                          &error);
        if (pkg == nullptr) {
            fprintf(stderr, "%s\n", error->message);
            g_error_free(error);
            return EXIT_FAILURE;
        }
        xd_theme_use_package(pkg, o.filter, o.integer);
    }

    xd_theme theme;
    xd_theme_init(&theme, o.style, &scheme, o.scale);
    const xd_theme *t = &theme;

    if (o.element != nullptr) {
        cairo_surface_t *s = draw_element(t, o.element, o.background);
        if (s == nullptr) {
            fprintf(stderr, "unknown element: %s\n", o.element);
            return EXIT_FAILURE;
        }
        cairo_status_t st = cairo_surface_write_to_png(s, o.out);
        cairo_surface_destroy(s);
        return st == CAIRO_STATUS_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    int width = xd_tpx(t, 640);
    int height = xd_tpx(t, 660);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(surface);

    xd_fill_rect(cr, (xd_rect){0, 0, width, height}, xd_sys(t, XD_COLOR_BACKGROUND));

    xd_frame back = {.title = "My Documents",
                     .active = false,
                     .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE};
    draw_window(t, cr, (xd_rect){xd_tpx(t, 24), xd_tpx(t, 20), xd_tpx(t, 300), xd_tpx(t, 200)},
                &back, false);

    xd_frame front = {.title = "Display Properties",
                      .active = true,
                      .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_HELP};
    draw_window(t, cr, (xd_rect){xd_tpx(t, 190), xd_tpx(t, 90), xd_tpx(t, 400), xd_tpx(t, 220)},
                &front, true);

    draw_controls(t, cr, (xd_rect){xd_tpx(t, 24), xd_tpx(t, 320), xd_tpx(t, 592), xd_tpx(t, 300)});

    draw_taskbar(t, cr, width, height);

    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surface, o.out);
    cairo_surface_destroy(surface);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "cannot write %s: %s\n", o.out, cairo_status_to_string(st));
        return EXIT_FAILURE;
    }
    printf("wrote %s (%dx%d, %s, %s, scale %.2f%s%s)\n", o.out, width, height,
           xd_visual_style_name(theme.style), scheme.name, o.scale, pkg ? ", package " : "",
           pkg ? xd_pkg_name(pkg) : "");
    xd_theme_use_package(nullptr, XD_FILTER_SMOOTH, true);
    xd_pkg_free(pkg);
    return EXIT_SUCCESS;
}
