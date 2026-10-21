/* SPDX-License-Identifier: MIT
 * Compares a built-in style with a theme package (docs/THEMES.md), part
 * by part: each element is drawn twice by the same xd_theme_* function,
 * once by the built-in style and once with the package attached, at the
 * same size and state. For each it writes the two side by side with a
 * heat map of their difference, and scores it (mean and max per-channel
 * difference, 0-255). Used to match our procedural styles to the real
 * ones; the packages are imported from Microsoft's msstyles, which are
 * only read here, never copied.
 *
 *   xd-theme-compare --package DIR|FILE.xdtheme [--variant ID]
 *                    [--style luna|aero|...] [--scheme FILE] [--glass 0|1]
 *                    [--scales 1,1.25,1.5,2] [--out DIR] [--only PREFIX]
 *                    [--no-images] [--raw] [--geometry] [--bench N]
 *
 * At scales above 1 the package is drawn with the Smooth filter, and at
 * whole-number scales with the Integer filter too. The built-in side uses
 * our own scheme (--scheme), the package side the scheme with the
 * package's [SysMetrics] over it, so differing system colours, sizes and
 * fonts show (and are listed) as differences.
 *
 * The taskbar, Start button, task buttons and tray aren't compared: the
 * package path doesn't draw them yet, so both sides would be the
 * built-in style.
 */
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../src/theme/pkg.h"
#include "../src/theme/theme.h"

typedef enum kind {
    K_FRAME,
    K_CAPBTN,
    K_BUTTON,
    K_CHECK,
    K_RADIO,
    K_GROUP,
    K_FIELD,
    K_DROPDOWN,
    K_SCROLL,
    K_TAB,
    K_TABPANEL,
} kind;

typedef struct element {
    char name[64];
    kind kind;
    uint8_t state;   /* xd_widget_state */
    bool active;     /* frames */
    bool maximized;  /* frames */
    uint8_t buttons; /* frames: xd_caption_buttons */
    xd_frame_part part, hot, pressed;
} element;

typedef struct options {
    const char *package, *variant, *scheme, *out, *only;
    xd_visual_style style;
    bool style_set, glass, images, geometry, raw;
    int bench;
    double scales[8];
    size_t nscales;
} options;

/* ---- the elements --------------------------------------------------------------- */

static element *elements;
static size_t n_elements, cap_elements;

static void add(element e) {
    if (n_elements == cap_elements) {
        cap_elements = cap_elements ? 2 * cap_elements : 64;
        elements = realloc(elements, cap_elements * sizeof(*elements));
        if (elements == nullptr)
            abort();
    }
    elements[n_elements++] = e;
}

static const struct {
    const char *name;
    uint8_t state;
} push_states[] = {
    {"normal", XD_STATE_NORMAL},     {"hot", XD_STATE_HOT},
    {"pressed", XD_STATE_PRESSED | XD_STATE_HOT},
    {"disabled", XD_STATE_DISABLED}, {"default", XD_STATE_DEFAULT},
    {"focused", XD_STATE_FOCUSED | XD_STATE_DEFAULT},
};

static void build_elements(void) {
    static const uint8_t three = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE;
    add((element){.name = "frame-active", .kind = K_FRAME, .active = true, .buttons = three});
    add((element){.name = "frame-inactive", .kind = K_FRAME, .buttons = three});
    add((element){.name = "frame-maximized",
                  .kind = K_FRAME,
                  .active = true,
                  .maximized = true,
                  .buttons = three});
    add((element){.name = "frame-dialog",
                  .kind = K_FRAME,
                  .active = true,
                  .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_HELP});

    static const struct {
        const char *name;
        xd_frame_part part;
        bool maximized;
    } btns[] = {
        {"close", XD_PART_CLOSE, false},       {"maximize", XD_PART_MAXIMIZE, false},
        {"minimize", XD_PART_MINIMIZE, false}, {"restore", XD_PART_MAXIMIZE, true},
        {"help", XD_PART_HELP, false},
    };
    static const char *const bstate[] = {"normal", "hot", "pressed"};
    for (size_t b = 0; b < XD_ARRAY_LEN(btns); b++) {
        for (int act = 1; act >= 0; act--) {
            for (int s = 0; s < 3; s++) {
                element e = {.kind = K_CAPBTN,
                             .active = act,
                             .maximized = btns[b].maximized,
                             .part = btns[b].part,
                             .buttons = btns[b].part == XD_PART_HELP
                                            ? XD_CAPBTN_CLOSE | XD_CAPBTN_HELP
                                            : three};
                if (s >= 1)
                    e.hot = btns[b].part;
                if (s == 2)
                    e.pressed = btns[b].part;
                snprintf(e.name, sizeof(e.name), "capbtn-%s-%s-%s", btns[b].name,
                         act ? "active" : "inactive", bstate[s]);
                add(e);
            }
        }
    }

    for (size_t i = 0; i < XD_ARRAY_LEN(push_states); i++) {
        element e = {.kind = K_BUTTON, .state = push_states[i].state};
        snprintf(e.name, sizeof(e.name), "button-%s", push_states[i].name);
        add(e);
    }
    static const struct {
        const char *name;
        uint8_t state;
    } box_states[] = {
        {"normal", 0},
        {"hot", XD_STATE_HOT},
        {"pressed", XD_STATE_PRESSED | XD_STATE_HOT},
        {"disabled", XD_STATE_DISABLED},
    };
    for (int checked = 0; checked < 2; checked++) {
        for (size_t i = 0; i < XD_ARRAY_LEN(box_states); i++) {
            uint8_t st = box_states[i].state | (checked ? XD_STATE_CHECKED : 0);
            element e = {.kind = K_CHECK, .state = st};
            snprintf(e.name, sizeof(e.name), "check-%s-%s", checked ? "on" : "off",
                     box_states[i].name);
            add(e);
            e.kind = K_RADIO;
            snprintf(e.name, sizeof(e.name), "radio-%s-%s", checked ? "on" : "off",
                     box_states[i].name);
            add(e);
        }
    }
    add((element){.name = "groupbox", .kind = K_GROUP});
    add((element){.name = "field-normal", .kind = K_FIELD});
    add((element){.name = "field-disabled", .kind = K_FIELD, .state = XD_STATE_DISABLED});
    for (size_t i = 0; i < 4; i++) {
        element e = {.kind = K_DROPDOWN, .state = box_states[i].state};
        snprintf(e.name, sizeof(e.name), "dropdown-%s", box_states[i].name);
        add(e);
    }
    add((element){.name = "scrollbar", .kind = K_SCROLL});
    add((element){.name = "scrollbar-disabled", .kind = K_SCROLL, .state = XD_STATE_DISABLED});
    add((element){.name = "tab-normal", .kind = K_TAB});
    add((element){.name = "tab-hot", .kind = K_TAB, .state = XD_STATE_HOT});
    add((element){.name = "tab-selected", .kind = K_TAB, .state = XD_STATE_CHECKED});
    add((element){.name = "tab-page", .kind = K_TABPANEL});
}

/* What the package must style for its side to differ from the built-in
 * one (else the package path falls back to it and the score means
 * nothing). */
static bool package_draws(const xd_theme *t, const element *e) {
    const xd_pkg *p = t->pkg;
    switch (e->kind) {
    case K_FRAME:
    case K_CAPBTN:
        return xd_pkg_has(p, "Window", "Caption") || xd_pkg_has(p, "DWMWindow", "CloseButton");
    case K_BUTTON:
        return xd_pkg_has(p, "Button", "PushButton");
    case K_CHECK:
        return xd_pkg_has(p, "Button", "CheckBox");
    case K_RADIO:
        return xd_pkg_has(p, "Button", "RadioButton");
    case K_GROUP:
        return xd_pkg_has(p, "Button", "GroupBox");
    case K_FIELD:
        return xd_pkg_has(p, "Edit", "EditText");
    case K_DROPDOWN:
        return xd_pkg_has(p, "ComboBox", "DropDownButton");
    case K_SCROLL:
        return xd_pkg_has(p, "ScrollBar", "ArrowBtn");
    case K_TAB:
        return xd_pkg_has(p, "Tab", "TabItem");
    case K_TABPANEL:
        return xd_pkg_has(p, "Tab", "Pane");
    }
    return false;
}

/* ---- drawing -------------------------------------------------------------------- */

enum { FRAME_W = 320, FRAME_H = 160 };

static xd_frame frame_of(const element *e) {
    return (xd_frame){.title = "Display Properties",
                      .active = e->active,
                      .maximized = e->maximized,
                      .dialog = e->buttons & XD_CAPBTN_HELP,
                      .buttons = e->buttons,
                      .hot = e->hot,
                      .pressed = e->pressed};
}

/* The element's size in device px (the same for both sides). */
static void element_size(const xd_theme *t, const element *e, int *w, int *h) {
    switch (e->kind) {
    case K_FRAME:
    case K_CAPBTN:
        *w = xd_tpx(t, FRAME_W);
        *h = xd_tpx(t, FRAME_H);
        return;
    case K_BUTTON:
        *w = xd_tpx(t, 75);
        *h = xd_tpx(t, 23);
        return;
    case K_CHECK:
    case K_RADIO:
        *w = xd_tpx(t, 60);
        *h = xd_tpx(t, 17);
        return;
    case K_GROUP:
        *w = xd_tpx(t, 140);
        *h = xd_tpx(t, 70);
        return;
    case K_FIELD:
        *w = xd_tpx(t, 100);
        *h = xd_tpx(t, 21);
        return;
    case K_DROPDOWN:
        *w = xd_tpx(t, 17);
        *h = xd_tpx(t, 19);
        return;
    case K_SCROLL:
        *w = xd_tpx(t, 17);
        *h = xd_tpx(t, 120);
        return;
    case K_TAB:
        *w = xd_tpx(t, 64);
        *h = xd_tpx(t, 21);
        return;
    case K_TABPANEL:
        *w = xd_tpx(t, 160);
        *h = xd_tpx(t, 80);
        return;
    }
}

static void draw(const xd_theme *t, cairo_t *cr, const element *e, int w, int h) {
    xd_rect r = {0, 0, w, h};
    switch (e->kind) {
    case K_FRAME:
    case K_CAPBTN: {
        xd_frame f = frame_of(e);
        xd_theme_draw_frame(t, cr, r, &f);
        return;
    }
    case K_BUTTON:
        xd_theme_draw_button(t, cr, r, e->state, "OK");
        return;
    case K_CHECK:
        xd_theme_draw_checkbox(t, cr, r, e->state, "Label");
        return;
    case K_RADIO:
        xd_theme_draw_radio(t, cr, r, e->state, "Label");
        return;
    case K_GROUP:
        xd_theme_draw_groupbox(t, cr, r, "Group");
        return;
    case K_FIELD:
        (void)xd_theme_draw_field(t, cr, r, e->state & XD_STATE_DISABLED);
        return;
    case K_DROPDOWN:
        xd_theme_draw_dropdown_button(t, cr, r, e->state);
        return;
    case K_SCROLL:
        if (e->state & XD_STATE_DISABLED)
            xd_theme_draw_vscrollbar(t, cr, r, 0, 10, 10);
        else
            xd_theme_draw_vscrollbar(t, cr, r, 5, 10, 30);
        return;
    case K_TAB:
        xd_theme_draw_tab(t, cr, r, e->state, "General");
        return;
    case K_TABPANEL:
        xd_theme_draw_tab_panel(t, cr, r);
        return;
    }
}


static bool button_rect(const xd_theme *t, const element *e, xd_rect *out) {
    xd_frame f = frame_of(e);
    xd_frame_layout l = {0};
    xd_theme_frame_layout(t, (xd_rect){0, 0, xd_tpx(t, FRAME_W), xd_tpx(t, FRAME_H)}, &f, &l);
    for (size_t i = 0; i < l.button_count; i++) {
        if (l.button_parts[i] == e->part) {
            *out = l.button_rects[i];
            return true;
        }
    }
    return false;
}

/* The element drawn by `t`, cut to `crop` (or whole if crop.w is 0). */
static cairo_surface_t *render(const xd_theme *t, const element *e, xd_rgba background,
                               xd_rect crop) {
    int w = 0, h = 0;
    element_size(t, e, &w, &h);
    cairo_surface_t *full = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(full);
    draw(t, cr, e, w, h);
    cairo_destroy(cr);
    if (crop.w <= 0)
        crop = (xd_rect){0, 0, w, h};
    /* Drawn on nothing, then over the background: Aero's frame replaces
     * what's under it with its glass. */
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, crop.w, crop.h);
    cr = cairo_create(s);
    xd_fill_rect(cr, (xd_rect){0, 0, crop.w, crop.h}, background);
    cairo_set_source_surface(cr, full, -crop.x, -crop.y);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(full);
    return s;
}

/* ---- comparing ------------------------------------------------------------------ */

typedef struct score {
    double mean;
    int max;
    double over; /* share of pixels differing by more than 32 in a channel */
} score;

static uint32_t pixel(cairo_surface_t *s, int x, int y) {
    const uint8_t *d = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    uint32_t v;
    memcpy(&v, d + (size_t)y * (size_t)stride + (size_t)x * 4, 4);
    return v;
}

/* Black (equal) through red and yellow to white (255 apart). */
static uint32_t heat(int d) {
    double v = sqrt(d / 255.0);
    double r = fmin(1, v * 3), g = fmin(1, fmax(0, v * 3 - 1)), b = fmax(0, v * 3 - 2);
    return 0xFF000000u | (uint32_t)lround(r * 255) << 16 | (uint32_t)lround(g * 255) << 8 |
           (uint32_t)lround(b * 255);
}

static score compare(cairo_surface_t *a, cairo_surface_t *b, cairo_surface_t *heatmap) {
    cairo_surface_flush(a);
    cairo_surface_flush(b);
    int w = cairo_image_surface_get_width(a), h = cairo_image_surface_get_height(a);
    uint8_t *hd = cairo_image_surface_get_data(heatmap);
    int hs = cairo_image_surface_get_stride(heatmap);
    double sum = 0;
    long over = 0;
    int max = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t pa = pixel(a, x, y), pb = pixel(b, x, y);
            int dmax = 0;
            for (int sh = 0; sh < 24; sh += 8) {
                int d = abs((int)((pa >> sh) & 0xFF) - (int)((pb >> sh) & 0xFF));
                sum += d;
                dmax = XD_MAX(dmax, d);
            }
            max = XD_MAX(max, dmax);
            if (dmax > 32)
                over++;
            uint32_t c = heat(dmax);
            memcpy(hd + (size_t)y * (size_t)hs + (size_t)x * 4, &c, 4);
        }
    }
    cairo_surface_mark_dirty(heatmap);
    double n = (double)w * h;
    return (score){.mean = sum / (3 * n), .max = max, .over = (double)over / n};
}

/* Built-in | package | difference, magnified for small parts. */
static void write_sheet(const char *path, cairo_surface_t *a, cairo_surface_t *b,
                        cairo_surface_t *d) {
    int w = cairo_image_surface_get_width(a), h = cairo_image_surface_get_height(a);
    int zoom = XD_CLAMP(240 / XD_MAX(w, h), 1, 8);
    int gap = 6;
    int sw = 3 * w * zoom + 4 * gap, sh = h * zoom + 2 * gap;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
    cairo_t *cr = cairo_create(s);
    xd_fill_rect(cr, (xd_rect){0, 0, sw, sh}, xd_rgba_hex(0x808080));
    cairo_surface_t *parts[] = {a, b, d};
    for (int i = 0; i < 3; i++) {
        cairo_save(cr);
        cairo_translate(cr, gap + i * (w * zoom + gap), gap);
        cairo_scale(cr, zoom, zoom);
        cairo_set_source_surface(cr, parts[i], 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    cairo_destroy(cr);
    if (cairo_surface_write_to_png(s, path) != CAIRO_STATUS_SUCCESS)
        fprintf(stderr, "cannot write %s\n", path);
    cairo_surface_destroy(s);
}

/* ---- geometry and system metrics --------------------------------------------------- */

static void print_geometry(FILE *out, const xd_theme *a, const xd_theme *b) {
    static const struct {
        const char *name;
        bool maximized, help;
    } frames[] = {{"normal", false, false}, {"maximized", true, false}, {"dialog", false, true}};
    fprintf(out, "\nGeometry (device px; built-in / package):\n\n");
    for (size_t i = 0; i < XD_ARRAY_LEN(frames); i++) {
        xd_frame f = {.active = true,
                      .maximized = frames[i].maximized,
                      .dialog = frames[i].help,
                      .buttons = frames[i].help ? XD_CAPBTN_CLOSE | XD_CAPBTN_HELP
                                                : XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE |
                                                      XD_CAPBTN_MINIMIZE};
        const xd_theme *th[2] = {a, b};
        xd_insets in[2];
        xd_frame_layout l[2] = {0};
        for (int k = 0; k < 2; k++) {
            in[k] = xd_theme_frame_insets(th[k], &f);
            xd_theme_frame_layout(
                th[k], (xd_rect){0, 0, xd_tpx(th[k], FRAME_W), xd_tpx(th[k], FRAME_H)}, &f,
                &l[k]);
        }
        fprintf(out, "- %s frame: insets l%d t%d r%d b%d / l%d t%d r%d b%d\n", frames[i].name,
                in[0].left, in[0].top, in[0].right, in[0].bottom, in[1].left, in[1].top,
                in[1].right, in[1].bottom);
        for (size_t j = 0; j < XD_MAX(l[0].button_count, l[1].button_count); j++) {
            xd_rect r0 = j < l[0].button_count ? l[0].button_rects[j] : (xd_rect){0};
            xd_rect r1 = j < l[1].button_count ? l[1].button_rects[j] : (xd_rect){0};
            fprintf(out, "  - button %zu: %d,%d %dx%d / %d,%d %dx%d\n", j, r0.x, r0.y, r0.w,
                    r0.h, r1.x, r1.y, r1.w, r1.h);
        }
        fprintf(out, "  - icon: %d,%d / %d,%d; title: x%d / x%d\n", l[0].icon.x, l[0].icon.y,
                l[1].icon.x, l[1].icon.y, l[0].title.x, l[1].title.x);
    }
}

static void print_sysmetrics(FILE *out, const xd_theme *a, const xd_theme *b) {
    fprintf(out, "\nSystem colours, metrics and fonts that differ (scheme / package):\n\n");
    int n = 0;
    for (int i = 0; i < XD_COLOR_COUNT; i++) {
        xd_rgb x = a->scheme.colors[i], y = b->scheme.colors[i];
        if (x.r != y.r || x.g != y.g || x.b != y.b) {
            fprintf(out, "- %s: %d %d %d / %d %d %d\n", xd_syscolor_name((xd_syscolor)i), x.r,
                    x.g, x.b, y.r, y.g, y.b);
            n++;
        }
    }
    static const char *const metric_names[] = {
#define XD_X(id, name, def) name,
        XD_METRICS(XD_X)
#undef XD_X
    };
    for (int i = 0; i < XD_METRIC_COUNT; i++) {
        if (a->scheme.metrics[i] != b->scheme.metrics[i]) {
            fprintf(out, "- %s: %d / %d\n", metric_names[i], a->scheme.metrics[i],
                    b->scheme.metrics[i]);
            n++;
        }
    }
    static const char *const font_names[] = {"CaptionFont", "MenuFont",    "MessageFont",
                                             "IconFont",    "StatusFont", "SmCaptionFont"};
    static_assert(XD_ARRAY_LEN(font_names) >= XD_FONT_COUNT);
    for (int i = 0; i < XD_FONT_COUNT; i++) {
        char fa[160], fb[160];
        xd_font_spec_format(&a->scheme.fonts[i], fa, sizeof(fa));
        xd_font_spec_format(&b->scheme.fonts[i], fb, sizeof(fb));
        if (strcmp(fa, fb) != 0) {
            fprintf(out, "- %s: %s / %s\n", font_names[i], fa, fb);
            n++;
        }
    }
    if (n == 0)
        fprintf(out, "- none\n");
}

/* ---- main ----------------------------------------------------------------------- */

static void usage(FILE *f) {
    fprintf(f, "usage: xd-theme-compare --package DIR [--variant ID] [--style NAME]\n"
               "                        [--scheme FILE] [--glass 0|1] [--scales 1,1.25,...]\n"
               "                        [--out DIR] [--only PREFIX] [--no-images] [--raw] [--geometry]\n"
               "                        [--bench N]\n");
}

static bool parse_args(int argc, char **argv, options *o) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0) {
            usage(stdout);
            exit(EXIT_SUCCESS);
        }
        if (strcmp(arg, "--no-images") == 0) {
            o->images = false;
            continue;
        }
        if (strcmp(arg, "--raw") == 0) {
            o->raw = true;
            continue;
        }
        if (strcmp(arg, "--geometry") == 0) {
            o->geometry = true;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n", arg);
            return false;
        }
        const char *val = argv[++i];
        if (strcmp(arg, "--package") == 0) {
            o->package = val;
        } else if (strcmp(arg, "--variant") == 0) {
            o->variant = val;
        } else if (strcmp(arg, "--scheme") == 0) {
            o->scheme = val;
        } else if (strcmp(arg, "--out") == 0) {
            o->out = val;
        } else if (strcmp(arg, "--only") == 0) {
            o->only = val;
        } else if (strcmp(arg, "--bench") == 0) {
            o->bench = atoi(val);
        } else if (strcmp(arg, "--glass") == 0) {
            o->glass = strcmp(val, "0") != 0;
        } else if (strcmp(arg, "--style") == 0) {
            if (!xd_visual_style_from_name(xd_str_from_cstr(val), &o->style)) {
                fprintf(stderr, "unknown style: %s\n", val);
                return false;
            }
            o->style_set = true;
        } else if (strcmp(arg, "--scales") == 0) {
            o->nscales = 0;
            char **parts = g_strsplit(val, ",", 8);
            for (size_t k = 0; parts[k] != nullptr; k++)
                o->scales[o->nscales++] = g_ascii_strtod(parts[k], nullptr);
            g_strfreev(parts);
        } else {
            fprintf(stderr, "unknown option: %s\n", arg);
            return false;
        }
    }
    if (o->package == nullptr) {
        fprintf(stderr, "--package is required\n");
        return false;
    }
    return true;
}

static void mkdirs(const char *path) {
    if (g_mkdir_with_parents(path, 0755) != 0)
        fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno));
}

typedef struct run {
    double scale;
    xd_image_filter filter;
    const char *filter_name;
} run;

int main(int argc, char **argv) {
    options o = {.out = "build-scratch/compare",
                 .images = true,
                 .scales = {1, 1.25, 1.5, 2},
                 .nscales = 4};
    if (!parse_args(argc, argv, &o)) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    /* Only what the command line says: no package from the environment. */
    g_unsetenv("XD_THEME_PACKAGE");

    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    if (o.scheme != nullptr) {
        xd_strbuf err = {0};
        if (!xd_scheme_load(o.scheme, &scheme, &err)) {
            fprintf(stderr, "%s: %s\n", o.scheme, xd_strbuf_cstr(&err));
            xd_strbuf_free(&err);
            return EXIT_FAILURE;
        }
        xd_strbuf_free(&err);
    }
    GError *error = nullptr;
    xd_pkg *pkg = xd_pkg_load(o.package, o.variant, XD_SOURCE_ROOT "/data/theme/uxtheme.ini",
                              &error);
    if (pkg == nullptr) {
        fprintf(stderr, "%s\n", error->message);
        g_error_free(error);
        return EXIT_FAILURE;
    }
    xd_visual_style style = o.style_set ? o.style : xd_pkg_base(pkg);

    build_elements();
    run runs[16];
    size_t nruns = 0;
    for (size_t i = 0; i < o.nscales && nruns + 2 <= XD_ARRAY_LEN(runs); i++) {
        double s = o.scales[i];
        runs[nruns++] = (run){s, XD_FILTER_SMOOTH, s == 1 ? "1x" : "smooth"};
        if (s > 1 && s == floor(s))
            runs[nruns++] = (run){s, XD_FILTER_INTEGER, "integer"};
    }

    mkdirs(o.out);
    char *md_path = g_build_filename(o.out, "scores.md", nullptr);
    FILE *md = fopen(md_path, "w");
    if (md == nullptr) {
        fprintf(stderr, "cannot write %s\n", md_path);
        return EXIT_FAILURE;
    }
    FILE *outs[2] = {stdout, md};
    for (int k = 0; k < 2; k++) {
        fprintf(outs[k], "# %s (%s) against %s, variant %s\n", xd_visual_style_name(style),
                scheme.name, xd_pkg_name(pkg), xd_pkg_variant(pkg));
        fprintf(outs[k], "\nScores: mean and max per-channel difference (0-255), and the share "
                         "of pixels more than 32 apart.\n\n| element |");
        for (size_t r = 0; r < nruns; r++)
            fprintf(outs[k], " %g %s |", runs[r].scale, runs[r].filter_name);
        fprintf(outs[k], "\n|---|");
        for (size_t r = 0; r < nruns; r++)
            fprintf(outs[k], "---|");
        fprintf(outs[k], "\n");
    }

    /* themes[r][0]: built-in, [r][1]: package. */
    xd_theme (*themes)[2] = calloc(nruns, sizeof(*themes));
    for (size_t r = 0; r < nruns; r++) {
        xd_theme_use_package(nullptr, XD_FILTER_SMOOTH, true);
        xd_theme_init(&themes[r][0], style, &scheme, runs[r].scale);
        xd_theme_use_package(pkg, runs[r].filter, runs[r].filter == XD_FILTER_INTEGER);
        xd_theme_init(&themes[r][1], style, &scheme, runs[r].scale);
        xd_theme_use_package(nullptr, XD_FILTER_SMOOTH, true);
        themes[r][0].glass = themes[r][1].glass = o.glass;
        char *dir = g_strdup_printf("%s/%g-%s", o.out, runs[r].scale, runs[r].filter_name);
        if (o.images || o.raw)
            mkdirs(dir);
        g_free(dir);
    }

    double group_sum[16][16] = {0};
    int group_n[16][16] = {0};
    static const char *const kind_names[] = {"frame",    "capbtn", "button",   "check",
                                             "radio",    "group",  "field",    "dropdown",
                                             "scroll",   "tab",    "tab-page"};
    for (size_t i = 0; i < n_elements; i++) {
        const element *e = &elements[i];
        if (o.only != nullptr && strncmp(e->name, o.only, strlen(o.only)) != 0)
            continue;
        char line[1024];
        int len = snprintf(line, sizeof(line), "| %s |", e->name);
        for (size_t r = 0; r < nruns; r++) {
            const xd_theme *a = &themes[r][0], *b = &themes[r][1];
            if (!package_draws(b, e)) {
                len += snprintf(line + len, sizeof(line) - (size_t)len, " (not in package) |");
                continue;
            }
            /* A caption button is cut from each side's frame where that
             * side puts it, so its looks are compared apart from where it
             * sits (--geometry lists that). */
            xd_rect crop_a = {0}, crop_b = {0};
            if (e->kind == K_CAPBTN) {
                bool ha = button_rect(a, e, &crop_a), hb = button_rect(b, e, &crop_b);
                if (!ha && !hb) {
                    len += snprintf(line + len, sizeof(line) - (size_t)len, " (none) |");
                    continue;
                }
                if (!ha)
                    crop_a = crop_b;
                if (!hb)
                    crop_b = crop_a;
                /* The same size around the middle of each, with room for a
                 * hot DWM button's glow. */
                int pad = xd_pkg_has(b->pkg, "DWMWindow", "CloseButton") && o.glass
                              ? xd_tpx(a, 3)
                              : 0;
                int cw = XD_MAX(crop_a.w, crop_b.w) + 2 * pad;
                int ch = XD_MAX(crop_a.h, crop_b.h) + 2 * pad;
                crop_a = (xd_rect){crop_a.x + crop_a.w / 2 - cw / 2,
                                   crop_a.y + crop_a.h / 2 - ch / 2, cw, ch};
                crop_b = (xd_rect){crop_b.x + crop_b.w / 2 - cw / 2,
                                   crop_b.y + crop_b.h / 2 - ch / 2, cw, ch};
            }
            xd_rgba bg = xd_theme_dialog_color(a);
            if (o.bench > 0) {
                /* How long the built-in style takes to draw it. */
                gint64 start = g_get_monotonic_time();
                for (int k = 0; k < o.bench; k++)
                    cairo_surface_destroy(render(a, e, bg, crop_a));
                fprintf(stderr, "%s at %g: %.2f ms\n", e->name, runs[r].scale,
                        (double)(g_get_monotonic_time() - start) / 1000.0 / o.bench);
            }
            cairo_surface_t *sa = render(a, e, bg, crop_a);
            cairo_surface_t *sb = render(b, e, bg, crop_b);
            int w = cairo_image_surface_get_width(sa), h = cairo_image_surface_get_height(sa);
            cairo_surface_t *sd = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            score sc = compare(sa, sb, sd);
            if (o.images) {
                char *path = g_strdup_printf("%s/%g-%s/%s.png", o.out, runs[r].scale,
                                             runs[r].filter_name, e->name);
                write_sheet(path, sa, sb, sd);
                g_free(path);
            }
            if (o.raw) {
                /* Each side alone, unmagnified, for measuring. */
                cairo_surface_t *sides[] = {sa, sb};
                static const char *const side_names[] = {"builtin", "package"};
                for (int k = 0; k < 2; k++) {
                    char *path = g_strdup_printf("%s/%g-%s/%s.%s.png", o.out, runs[r].scale,
                                                 runs[r].filter_name, e->name, side_names[k]);
                    (void)cairo_surface_write_to_png(sides[k], path);
                    g_free(path);
                }
            }
            cairo_surface_destroy(sa);
            cairo_surface_destroy(sb);
            cairo_surface_destroy(sd);
            len += snprintf(line + len, sizeof(line) - (size_t)len, " %.1f / %d / %.0f%% |",
                            sc.mean, sc.max, 100 * sc.over);
            group_sum[e->kind][r] += sc.mean;
            group_n[e->kind][r]++;
        }
        fprintf(stdout, "%s\n", line);
        fprintf(md, "%s\n", line);
    }

    for (int k = 0; k < 2; k++) {
        fprintf(outs[k], "\nMean difference by kind:\n\n| kind |");
        for (size_t r = 0; r < nruns; r++)
            fprintf(outs[k], " %g %s |", runs[r].scale, runs[r].filter_name);
        fprintf(outs[k], "\n|---|");
        for (size_t r = 0; r < nruns; r++)
            fprintf(outs[k], "---|");
        fprintf(outs[k], "\n");
        for (size_t g = 0; g < XD_ARRAY_LEN(kind_names); g++) {
            if (group_n[g][0] == 0)
                continue;
            fprintf(outs[k], "| %s |", kind_names[g]);
            for (size_t r = 0; r < nruns; r++)
                fprintf(outs[k], " %.1f |", group_n[g][r] ? group_sum[g][r] / group_n[g][r] : 0);
            fprintf(outs[k], "\n");
        }
        fprintf(outs[k], "\nNot compared: the taskbar, Start button, task buttons and tray "
                         "(the package path doesn't draw them).\n");
        if (o.geometry)
            print_geometry(outs[k], &themes[0][0], &themes[0][1]);
        print_sysmetrics(outs[k], &themes[0][0], &themes[0][1]);
    }

    fclose(md);
    g_free(md_path);
    free(themes);
    free(elements);
    xd_pkg_free(pkg);
    return EXIT_SUCCESS;
}
