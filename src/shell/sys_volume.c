/* SPDX-License-Identifier: MIT
 * The volume icon in the notification area, through libpulse (PulseAudio
 * or PipeWire's pulse server) on GLib's main loop: the default output's
 * volume and mute. A click opens XP's volume flyout (a vertical slider
 * and Mute); the wheel over the icon changes the volume; the menu opens
 * the full mixer (pavucontrol) when it's installed.
 */
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

#include "../core/alloc.h"
#include "../theme/draw.h"
#include "shell.h"

typedef struct flyout flyout;

typedef struct volume {
    xd_shell *shell;
    pa_glib_mainloop *loop;
    pa_context *ctx;
    guint retry;
    bool ready;   /* a default output is known */
    char sink[256];
    pa_cvolume cv;
    bool muted;
    flyout *fly;
    int64_t closed_ms; /* when the flyout last closed */
} volume;

static volume *the_volume; /* for the flyout's pointer routing */

static int percent(const volume *v) {
    return (int)lround(pa_cvolume_max(&v->cv) * 100.0 / PA_VOLUME_NORM);
}

/* ---- libpulse ------------------------------------------------------------------ */

static void sink_info(pa_context *c, const pa_sink_info *info, int eol, void *data);
static void first_sink(pa_context *c, const pa_sink_info *info, int eol, void *data);
static void connect_server(volume *v);

static void server_info([[maybe_unused]] pa_context *c, const pa_server_info *info, void *data) {
    volume *v = data;
    if (info == nullptr || info->default_sink_name == nullptr) {
        v->ready = false;
        xd_tray_changed(v->shell);
        return;
    }
    g_strlcpy(v->sink, info->default_sink_name, sizeof(v->sink));
    pa_operation *op = pa_context_get_sink_info_by_name(v->ctx, v->sink, sink_info, v);
    if (op != nullptr)
        pa_operation_unref(op);
}

static void flyout_redraw(volume *v);

static void first_sink([[maybe_unused]] pa_context *c, const pa_sink_info *info, int eol,
                       void *data) {
    volume *v = data;
    if (eol != 0 || info == nullptr || v->ready)
        return;
    g_strlcpy(v->sink, info->name, sizeof(v->sink));
    sink_info(c, info, 0, v);
}

static void sink_info([[maybe_unused]] pa_context *c, const pa_sink_info *info, int eol,
                      void *data) {
    volume *v = data;
    if (eol < 0) {
        /* No usable default output (no session manager to pick one):
         * the first output will do. */
        v->ready = false;
        pa_operation *op = pa_context_get_sink_info_list(v->ctx, first_sink, v);
        if (op != nullptr)
            pa_operation_unref(op);
        return;
    }
    if (eol != 0 || info == nullptr)
        return;
    v->cv = info->volume;
    v->muted = info->mute != 0;
    v->ready = true;
    xd_tray_changed(v->shell);
    flyout_redraw(v);
}

static void query(volume *v) {
    pa_operation *op = pa_context_get_server_info(v->ctx, server_info, v);
    if (op != nullptr)
        pa_operation_unref(op);
}

static void subscribed([[maybe_unused]] pa_context *c, [[maybe_unused]] pa_subscription_event_type_t t,
                       [[maybe_unused]] uint32_t index, void *data) {
    query(data); /* the default output or its volume changed */
}

static gboolean retry_connect(gpointer data) {
    volume *v = data;
    v->retry = 0;
    connect_server(v);
    return G_SOURCE_REMOVE;
}

static void state_changed(pa_context *c, void *data) {
    volume *v = data;
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY: {
        pa_context_set_subscribe_callback(c, subscribed, v);
        pa_operation *op = pa_context_subscribe(
            c, (pa_subscription_mask_t)(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER),
            nullptr, nullptr);
        if (op != nullptr)
            pa_operation_unref(op);
        query(v);
        break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        /* The sound server went away: try again in a while. */
        v->ready = false;
        xd_tray_changed(v->shell);
        pa_context_unref(v->ctx);
        v->ctx = nullptr;
        if (v->retry == 0)
            v->retry = g_timeout_add_seconds(5, retry_connect, v);
        break;
    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
        break;
    }
}

static void connect_server(volume *v) {
    v->ctx = pa_context_new(pa_glib_mainloop_get_api(v->loop), "XeonDesktop");
    if (v->ctx == nullptr)
        return;
    pa_context_set_state_callback(v->ctx, state_changed, v);
    if (pa_context_connect(v->ctx, nullptr, PA_CONTEXT_NOFAIL, nullptr) < 0)
        state_changed(v->ctx, v);
}

static void set_percent(volume *v, int pct) {
    if (!v->ready || v->ctx == nullptr)
        return;
    pct = XD_CLAMP(pct, 0, 100);
    pa_cvolume cv = v->cv;
    /* Keeps the balance between channels, as XP's slider does. */
    pa_cvolume_scale(&cv, (pa_volume_t)lround(pct * (double)PA_VOLUME_NORM / 100.0));
    v->cv = cv;
    pa_operation *op = pa_context_set_sink_volume_by_name(v->ctx, v->sink, &cv, nullptr, nullptr);
    if (op != nullptr)
        pa_operation_unref(op);
    if (v->muted && pct > 0) {
        op = pa_context_set_sink_mute_by_name(v->ctx, v->sink, 0, nullptr, nullptr);
        if (op != nullptr)
            pa_operation_unref(op);
    }
    xd_tray_changed(v->shell);
    flyout_redraw(v);
}

static void set_mute(volume *v, bool mute) {
    if (!v->ready || v->ctx == nullptr)
        return;
    v->muted = mute;
    pa_operation *op = pa_context_set_sink_mute_by_name(v->ctx, v->sink, mute, nullptr, nullptr);
    if (op != nullptr)
        pa_operation_unref(op);
    xd_tray_changed(v->shell);
    flyout_redraw(v);
}

/* ---- the flyout ------------------------------------------------------------------ */

/* Logical px. */
static constexpr int FLY_W = 72;
static constexpr int FLY_H = 158;
static constexpr int TRACK_TOP = 32;
static constexpr int TRACK_BOTTOM = 112;
static constexpr int THUMB_W = 22;
static constexpr int THUMB_H = 11;

struct flyout {
    volume *vol;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *popup;
    struct wp_viewport *viewport;
    xd_shm_ring ring;
    double scale;
    bool configured, dirty;
    bool dragging;
    double wheel;
};

static xd_rect mute_rect(void) { return (xd_rect){10, FLY_H - 30, FLY_W - 16, 18}; }

static int value_at(double y) {
    double f = (TRACK_BOTTOM - y) / (double)(TRACK_BOTTOM - TRACK_TOP);
    return (int)lround(XD_CLAMP(f, 0.0, 1.0) * 100.0);
}

static void flyout_render(flyout *f) {
    xd_shell *shell = f->vol->shell;
    double s = f->scale;
    double dwd = ceil(FLY_W * s), dhd = ceil(FLY_H * s);
    int dw = (int)dwd, dh = (int)dhd;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&f->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme t;
    xd_theme_init(&t, shell->style, &shell->scheme, s);
    cairo_t *cr = cairo_create(buf->surface);
    xd_rect all = {0, 0, dw, dh};
    xd_fill_rect(cr, all, xd_sys(&t, XD_COLOR_BUTTON_FACE));
    int px = xd_tpx(&t, 1);
    /* A raised edge, like a small dialog. */
    xd_rect inner = xd_draw_bevel(cr, all, px, xd_sys(&t, XD_COLOR_BUTTON_LIGHT),
                                  xd_sys(&t, XD_COLOR_BUTTON_DK_SHADOW));
    xd_draw_bevel(cr, inner, px, xd_sys(&t, XD_COLOR_BUTTON_HILIGHT),
                  xd_sys(&t, XD_COLOR_BUTTON_SHADOW));

    xd_text_style title = xd_text(&t, XD_FONT_MENU, xd_sys(&t, XD_COLOR_BUTTON_TEXT),
                                  XD_ALIGN_CENTER);
    xd_draw_text(cr, (xd_rect){0, xd_tpx(&t, 6), dw, xd_tpx(&t, 16)}, "Volume", &title);

    /* The track: a sunken groove with tick marks at the left. */
    int cx = dw / 2;
    int top = xd_tpx(&t, TRACK_TOP), bottom = xd_tpx(&t, TRACK_BOTTOM);
    xd_rect groove = {cx - 2 * px, top - 2 * px, 4 * px, bottom - top + 4 * px};
    xd_rect g2 = xd_draw_bevel(cr, groove, px, xd_sys(&t, XD_COLOR_BUTTON_SHADOW),
                               xd_sys(&t, XD_COLOR_BUTTON_HILIGHT));
    xd_draw_bevel(cr, g2, px, xd_sys(&t, XD_COLOR_BUTTON_DK_SHADOW),
                  xd_sys(&t, XD_COLOR_BUTTON_LIGHT));
    for (int i = 0; i <= 10; i++) {
        int ty = top + (bottom - top) * i / 10;
        xd_fill_rect(cr, (xd_rect){cx - xd_tpx(&t, 17), ty, xd_tpx(&t, 4), px},
                     xd_sys(&t, XD_COLOR_BUTTON_TEXT));
    }
    int pct = percent(f->vol);
    int thumb_y = bottom - (bottom - top) * pct / 100;
    xd_rect thumb = {cx - xd_tpx(&t, THUMB_W) / 2, thumb_y - xd_tpx(&t, THUMB_H) / 2,
                     xd_tpx(&t, THUMB_W), xd_tpx(&t, THUMB_H)};
    if (t.style == XD_STYLE_CLASSIC) {
        xd_fill_rect(cr, thumb, xd_sys(&t, XD_COLOR_BUTTON_FACE));
        xd_rect ti = xd_draw_bevel(cr, thumb, px, xd_sys(&t, XD_COLOR_BUTTON_HILIGHT),
                                   xd_sys(&t, XD_COLOR_BUTTON_DK_SHADOW));
        xd_draw_bevel(cr, ti, px, xd_sys(&t, XD_COLOR_BUTTON_LIGHT),
                      xd_sys(&t, XD_COLOR_BUTTON_SHADOW));
    } else {
        /* XP's trackbar thumb: light, with a blue-grey outline. */
        xd_fill_rect(cr, thumb, xd_rgba_hex(0xF3F3EE));
        xd_draw_bevel(cr, thumb, px, xd_rgba_hex(0x7F9DB9), xd_rgba_hex(0x7F9DB9));
        xd_fill_rect(cr, (xd_rect){thumb.x + px, thumb.y + thumb.h - 2 * px, thumb.w - 2 * px, px},
                     xd_rgba_hex(0x21A121));
    }

    xd_rect m = mute_rect();
    xd_rect mr = {xd_tpx(&t, m.x), xd_tpx(&t, m.y), xd_tpx(&t, m.w), xd_tpx(&t, m.h)};
    xd_theme_draw_checkbox(&t, cr, mr, f->vol->muted ? XD_STATE_CHECKED : 0, "Mute");
    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);
    wl_surface_attach(f->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(f->surface, 0, 0, dw, dh);
    if (f->viewport != nullptr)
        wp_viewport_set_destination(f->viewport, FLY_W, FLY_H);
    wl_surface_commit(f->surface);
    buf->busy = true;
    f->dirty = false;
}

static void flyout_close(volume *v) {
    flyout *f = v->fly;
    if (f == nullptr)
        return;
    v->fly = nullptr;
    v->closed_ms = g_get_monotonic_time() / 1000;
    xdg_popup_destroy(f->popup);
    xdg_surface_destroy(f->xdg_surface);
    if (f->viewport != nullptr)
        wp_viewport_destroy(f->viewport);
    wl_surface_destroy(f->surface);
    xd_shm_ring_finish(&f->ring);
    xd_free(f);
    xd_taskbars_schedule_tuck(v->shell);
}

static void flyout_redraw(volume *v) {
    if (v->fly != nullptr)
        v->fly->dirty = true;
}

static void fly_configure(void *data, struct xdg_surface *s, uint32_t serial) {
    flyout *f = data;
    xdg_surface_ack_configure(s, serial);
    f->configured = true;
    f->dirty = true;
}

static const struct xdg_surface_listener fly_surface_listener = {.configure = fly_configure};

static void fly_popup_configure([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *p,
                                [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                                [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h) {}

static void fly_popup_done(void *data, [[maybe_unused]] struct xdg_popup *p) {
    flyout *f = data;
    flyout_close(f->vol); /* a click elsewhere */
}

static void fly_repositioned([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *p,
                             [[maybe_unused]] uint32_t token) {}

static const struct xdg_popup_listener fly_popup_listener = {
    .configure = fly_popup_configure,
    .popup_done = fly_popup_done,
    .repositioned = fly_repositioned,
};

static void flyout_open(volume *v, xd_taskbar *bar, xd_rect anchor, uint32_t serial) {
    xd_shell *shell = v->shell;
    xd_menu_close_all(shell);
    /* A second click on the icon closes it: the press already did. */
    if (v->fly != nullptr) {
        flyout_close(v);
        return;
    }
    if (g_get_monotonic_time() / 1000 - v->closed_ms < 250)
        return;
    flyout *f = XD_NEW(flyout);
    f->vol = v;
    f->scale = bar->scale;
    f->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->viewporter != nullptr)
        f->viewport = wp_viewporter_get_viewport(shell->viewporter, f->surface);
    else
        f->scale = 1.0;
    f->xdg_surface = xdg_wm_base_get_xdg_surface(shell->wm_base, f->surface);
    xdg_surface_add_listener(f->xdg_surface, &fly_surface_listener, f);
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, FLY_W, FLY_H);
    xdg_positioner_set_anchor_rect(pos, anchor.x, anchor.y, XD_MAX(anchor.w, 1),
                                   XD_MAX(anchor.h, 1));
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_TOP);
    xdg_positioner_set_constraint_adjustment(pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                                      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
    f->popup = xdg_surface_get_popup(f->xdg_surface, nullptr, pos);
    xdg_positioner_destroy(pos);
    zwlr_layer_surface_v1_get_popup(bar->layer, f->popup);
    xdg_popup_add_listener(f->popup, &fly_popup_listener, f);
    if (shell->seat != nullptr && serial != 0)
        xdg_popup_grab(f->popup, shell->seat, serial);
    wl_surface_commit(f->surface);
    v->fly = f;
}

bool xd_volume_flyout_owns(xd_shell *shell, struct wl_surface *surface) {
    (void)shell;
    return the_volume != nullptr && the_volume->fly != nullptr &&
           the_volume->fly->surface == surface;
}

void xd_volume_flyout_motion([[maybe_unused]] xd_shell *shell, [[maybe_unused]] double x,
                             double y) {
    volume *v = the_volume;
    if (v != nullptr && v->fly != nullptr && v->fly->dragging)
        set_percent(v, value_at(y));
}

void xd_volume_flyout_button([[maybe_unused]] xd_shell *shell, uint32_t button, bool pressed,
                             double x, double y) {
    volume *v = the_volume;
    if (v == nullptr || v->fly == nullptr || button != BTN_LEFT)
        return;
    flyout *f = v->fly;
    if (!pressed) {
        f->dragging = false;
        return;
    }
    xd_rect m = mute_rect();
    if (x >= m.x && x < m.x + m.w && y >= m.y && y < m.y + m.h) {
        set_mute(v, !v->muted);
    } else if (y >= TRACK_TOP - THUMB_H && y <= TRACK_BOTTOM + THUMB_H) {
        f->dragging = true;
        set_percent(v, value_at(y));
    }
}

void xd_volume_flyout_dismiss([[maybe_unused]] xd_shell *shell) {
    if (the_volume != nullptr)
        flyout_close(the_volume);
}

void xd_volume_flyout_render_dirty([[maybe_unused]] xd_shell *shell) {
    volume *v = the_volume;
    if (v != nullptr && v->fly != nullptr && v->fly->configured && v->fly->dirty)
        flyout_render(v->fly);
}

/* ---- the icon ---------------------------------------------------------------------- */

static const char *icon(void *data) {
    volume *v = data;
    if (!v->ready)
        return nullptr; /* no sound server or output */
    int p = percent(v);
    return v->muted || p == 0 ? "audio-volume-muted"
           : p < 34          ? "audio-volume-low"
           : p < 67          ? "audio-volume-medium"
                             : "audio-volume-high";
}

static void draw(void *data, const xd_theme *t, cairo_t *cr, xd_rect r) {
    volume *v = data;
    int p = percent(v);
    int waves = p == 0 ? 0 : p < 34 ? 1 : p < 67 ? 2 : 3;
    xd_theme_draw_tray_icon(t, cr, r, XD_TRAY_ICON_VOLUME, waves,
                            v->muted || p == 0 ? XD_TRAY_ICON_OFF : 0);
}

static char *tooltip(void *data) {
    volume *v = data;
    return v->muted ? g_strdup("Volume: muted") : g_strdup_printf("Volume: %d%%", percent(v));
}

static void activate(void *data, xd_taskbar *bar, xd_rect anchor, uint32_t serial) {
    flyout_open(data, bar, anchor, serial);
}

static void scroll(void *data, double delta) {
    volume *v = data;
    set_percent(v, percent(v) - (int)lround(delta * 5)); /* wheel up: louder */
}

static void act_mixer(xd_shell *shell, [[maybe_unused]] void *d) {
    xd_shell_launch(shell, "pavucontrol");
}

static void act_mute(xd_shell *shell, void *d) {
    (void)shell;
    volume *v = d;
    set_mute(v, !v->muted);
}

static void menu(void *data, xd_menu_tree *tree, xd_menu_model *m) {
    volume *v = data;
    xd_menu_item *it = xd_menu_add(tree, m, XD_MENU_ACTION, "Open Volume Control", nullptr);
    it->action = act_mixer;
    it->bold = true;
    it->disabled = !xd_shell_have_program("pavucontrol");
    it = xd_menu_add(tree, m, XD_MENU_ACTION, "Mute", nullptr);
    it->action = act_mute;
    it->action_data = v;
    it->checked = v->muted;
}

static void destroy(void *data) {
    volume *v = data;
    flyout_close(v);
    if (v->retry != 0)
        g_source_remove(v->retry);
    if (v->ctx != nullptr) {
        pa_context_set_state_callback(v->ctx, nullptr, nullptr);
        pa_context_disconnect(v->ctx);
        pa_context_unref(v->ctx);
    }
    pa_glib_mainloop_free(v->loop);
    if (the_volume == v)
        the_volume = nullptr;
    g_free(v);
}

static const xd_sysicon_ops ops = {
    .icon = icon,
    .tooltip = tooltip,
    .menu = menu,
    .activate = activate,
    .scroll = scroll,
    .draw = draw,
    .destroy = destroy,
};

void xd_sys_volume_init(xd_shell *shell) {
    volume *v = g_new0(volume, 1);
    v->shell = shell;
    v->loop = pa_glib_mainloop_new(nullptr);
    the_volume = v;
    xd_tray_add_sysicon(shell, "volume", &ops, v);
    connect_server(v);
}
