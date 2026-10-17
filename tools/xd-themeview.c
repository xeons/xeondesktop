/* SPDX-License-Identifier: MIT
 * Renders a preview of a theme to PNG: an inactive and an active window,
 * dialog controls, and a taskbar. Used to check styles and DPI scaling
 * without a running session.
 *
 *   xd-themeview [--style classic|luna|whistler] [--scheme FILE]
 *                [--scale N] [--out FILE.png]
 *                [--element active-frame|inactive-frame|taskbar]
 *
 * --element draws just one element, at the size it has in the Windows XP
 * reference screenshot tools/luna-measure.py reads, over magenta, for
 * pixel comparisons.
 */
#include <stdlib.h>
#include <string.h>

#include "../src/theme/theme.h"

typedef struct options {
    xd_visual_style style;
    const char *scheme_path;
    double scale;
    const char *out;
    const char *element;
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
        } else if (xd_str_eq(arg, XD_STR("--out"))) {
            o->out = val.ptr;
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
static cairo_surface_t *draw_element(const xd_theme *t, const char *name) {
    bool active = strcmp(name, "active-frame") == 0;
    if (active || strcmp(name, "inactive-frame") == 0) {
        int w = xd_tpx(t, active ? 553 : 800), h = xd_tpx(t, active ? 323 : 600);
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t *cr = cairo_create(s);
        xd_fill_rect(cr, (xd_rect){0, 0, w, h}, xd_rgba_hex(0xFF00FF));
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
    options o = {.style = XD_STYLE_CLASSIC, .scale = 1.0, .out = "themeview.png"};
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

    xd_theme theme;
    xd_theme_init(&theme, o.style, &scheme, o.scale);
    const xd_theme *t = &theme;

    if (o.element != nullptr) {
        cairo_surface_t *s = draw_element(t, o.element);
        if (s == nullptr) {
            fprintf(stderr, "unknown element: %s\n", o.element);
            return EXIT_FAILURE;
        }
        cairo_status_t st = cairo_surface_write_to_png(s, o.out);
        cairo_surface_destroy(s);
        return st == CAIRO_STATUS_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    int width = xd_tpx(t, 640);
    int height = xd_tpx(t, 420);
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

    draw_taskbar(t, cr, width, height);

    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surface, o.out);
    cairo_surface_destroy(surface);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "cannot write %s: %s\n", o.out, cairo_status_to_string(st));
        return EXIT_FAILURE;
    }
    printf("wrote %s (%dx%d, %s, %s, scale %.2f)\n", o.out, width, height,
           xd_visual_style_name(o.style), scheme.name, o.scale);
    return EXIT_SUCCESS;
}
