/* SPDX-License-Identifier: MIT
 * Display Properties' Identify: each monitor's number, big, on that
 * monitor for a few seconds, so the user can tell which icon in the
 * Settings tab is which screen. Display Properties asks over D-Bus
 * (shellbus.c) with each monitor's output name and number; the number
 * goes in an overlay layer surface of its own on that output, which takes
 * no input.
 *
 * Windows draws a huge white numeral outlined in black near the screen's
 * top left corner; so does this.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>

#include "../core/alloc.h"
#include "shell.h"

/* Shown this long, in seconds. */
static constexpr guint SHOW_SECONDS = 3;
/* The numeral's em size and the space around it, logical px. */
static constexpr int FONT_PX = 220;
static constexpr int MARGIN = 24;
static constexpr int PAD = 12;

typedef struct identify_panel {
    struct wl_list link; /* xd_identify.panels */
    xd_shell *shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct wp_fractional_scale_v1 *fractional;
    struct wp_viewport *viewport;
    xd_shm_ring ring;
    char text[16];
    int width, height; /* logical */
    double scale;
    bool configured;
} identify_panel;

struct xd_identify {
    struct wl_list panels;
    guint timer;
};

static void set_font(cairo_t *cairo, double px) {
    cairo_select_font_face(cairo, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cairo, px);
}

/* The numeral's ink at scale 1, for the surface's size. */
static void measure(const char *text, cairo_text_extents_t *ext) {
    cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cairo = cairo_create(probe);
    set_font(cairo, FONT_PX);
    cairo_text_extents(cairo, text, ext);
    cairo_destroy(cairo);
    cairo_surface_destroy(probe);
}

static void render(identify_panel *panel) {
    xd_shell *shell = panel->shell;
    double fw = ceil(panel->width * panel->scale);
    double fh = ceil(panel->height * panel->scale);
    int dw = (int)fw;
    int dh = (int)fh;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&panel->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;
    cairo_t *cairo = cairo_create(buf->surface);
    cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
    cairo_scale(cairo, panel->scale, panel->scale);
    cairo_text_extents_t ext;
    measure(panel->text, &ext);
    set_font(cairo, FONT_PX);
    cairo_move_to(cairo, PAD - ext.x_bearing, PAD - ext.y_bearing);
    cairo_text_path(cairo, panel->text);
    cairo_set_source_rgb(cairo, 1, 1, 1);
    cairo_fill_preserve(cairo);
    cairo_set_source_rgb(cairo, 0, 0, 0);
    cairo_set_line_width(cairo, 4);
    cairo_set_line_join(cairo, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cairo);
    cairo_destroy(cairo);
    cairo_surface_flush(buf->surface);
    wl_surface_attach(panel->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(panel->surface, 0, 0, dw, dh);
    if (panel->viewport != nullptr)
        wp_viewport_set_destination(panel->viewport, panel->width, panel->height);
    wl_surface_commit(panel->surface);
    buf->busy = true;
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
                            [[maybe_unused]] uint32_t width, [[maybe_unused]] uint32_t height) {
    identify_panel *panel = data;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    panel->configured = true;
    render(panel);
}

static void layer_closed(void *data, [[maybe_unused]] struct zwlr_layer_surface_v1 *layer) {
    /* Its monitor went away: nothing more is drawn; the timer removes it. */
    ((identify_panel *)data)->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void preferred_scale(void *data,
                            [[maybe_unused]] struct wp_fractional_scale_v1 *fractional_scale,
                            uint32_t scale_120) {
    identify_panel *panel = data;
    double scale = scale_120 / 120.0;
    if (scale < 0.5 || scale > 8.0 || scale == panel->scale)
        return;
    panel->scale = scale;
    if (panel->configured)
        render(panel);
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = preferred_scale,
};

static void panel_destroy(identify_panel *panel) {
    wl_list_remove(&panel->link);
    zwlr_layer_surface_v1_destroy(panel->layer);
    if (panel->viewport != nullptr)
        wp_viewport_destroy(panel->viewport);
    if (panel->fractional != nullptr)
        wp_fractional_scale_v1_destroy(panel->fractional);
    wl_surface_destroy(panel->surface);
    xd_shm_ring_finish(&panel->ring);
    xd_free(panel);
}

static void panel_create(xd_shell *shell, xd_identify *id, struct wl_output *output,
                         unsigned number) {
    identify_panel *panel = XD_NEW(identify_panel);
    panel->shell = shell;
    panel->scale = 1.0;
    snprintf(panel->text, sizeof(panel->text), "%u", number);
    cairo_text_extents_t ext;
    measure(panel->text, &ext);
    double ink_w = ceil(ext.width);
    double ink_h = ceil(ext.height);
    panel->width = (int)ink_w + 2 * PAD;
    panel->height = (int)ink_h + 2 * PAD;

    panel->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->fractional_manager != nullptr && shell->viewporter != nullptr) {
        panel->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
            shell->fractional_manager, panel->surface);
        wp_fractional_scale_v1_add_listener(panel->fractional, &fractional_listener, panel);
        panel->viewport = wp_viewporter_get_viewport(shell->viewporter, panel->surface);
    }
    /* Clicks go through to whatever is under it. */
    struct wl_region *none = wl_compositor_create_region(shell->compositor);
    wl_surface_set_input_region(panel->surface, none);
    wl_region_destroy(none);
    panel->layer = zwlr_layer_shell_v1_get_layer_surface(
        shell->layer_shell, panel->surface, output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "identify");
    zwlr_layer_surface_v1_set_anchor(panel->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_size(panel->layer, (uint32_t)panel->width, (uint32_t)panel->height);
    zwlr_layer_surface_v1_set_margin(panel->layer, MARGIN, 0, 0, MARGIN);
    zwlr_layer_surface_v1_set_exclusive_zone(panel->layer, -1); /* over the taskbar too */
    zwlr_layer_surface_v1_add_listener(panel->layer, &layer_listener, panel);
    wl_surface_commit(panel->surface);
    wl_list_insert(id->panels.prev, &panel->link);
}

void xd_identify_hide(xd_shell *shell) {
    xd_identify *id = shell->identify;
    if (id == nullptr)
        return;
    shell->identify = nullptr;
    if (id->timer != 0)
        g_source_remove(id->timer);
    XD_LIST_FOR_EACH_SAFE(panel, tmp, &id->panels, identify_panel, link) { panel_destroy(panel); }
    xd_free(id);
}

static gboolean hide_timeout(gpointer data) {
    xd_shell *shell = data;
    shell->identify->timer = 0;
    xd_identify_hide(shell);
    return G_SOURCE_REMOVE;
}

void xd_identify_show(xd_shell *shell, const char *const *outputs, const unsigned *numbers,
                      size_t count) {
    xd_identify_hide(shell);
    if (shell->layer_shell == nullptr || count == 0)
        return;
    xd_identify *id = XD_NEW(xd_identify);
    wl_list_init(&id->panels);
    shell->identify = id;
    for (size_t i = 0; i < count; i++) {
        struct wl_output *output = xd_primary_output_named(shell, outputs[i]);
        if (output != nullptr)
            panel_create(shell, id, output, numbers[i]);
    }
    id->timer = g_timeout_add_seconds(SHOW_SECONDS, hide_timeout, shell);
}
