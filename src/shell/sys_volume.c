/* SPDX-License-Identifier: MIT
 * The volume icon in the notification area, through libpulse (PulseAudio
 * or PipeWire's pulse server) on GLib's main loop: the default output's
 * volume and mute. A click opens XP's volume flyout (a vertical slider
 * and Mute), which works from the keyboard as XP's did; the wheel over
 * the icon changes the volume; the menu opens the full mixer
 * (pavucontrol) when it's installed.
 */
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>
#include <xkbcommon/xkbcommon-keysyms.h>

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

static int percent(const volume *volume_icon) {
    return (int)lround(pa_cvolume_max(&volume_icon->cv) * 100.0 / PA_VOLUME_NORM);
}

/* ---- libpulse ------------------------------------------------------------------ */

static void sink_info(pa_context *context, const pa_sink_info *info, int eol, void *data);
static void first_sink(pa_context *context, const pa_sink_info *info, int eol, void *data);
static void connect_server(volume *volume_icon);

static void server_info([[maybe_unused]] pa_context *context, const pa_server_info *info,
                        void *data) {
    volume *volume_icon = data;
    if (info == nullptr || info->default_sink_name == nullptr) {
        volume_icon->ready = false;
        xd_tray_changed(volume_icon->shell);
        return;
    }
    g_strlcpy(volume_icon->sink, info->default_sink_name, sizeof(volume_icon->sink));
    pa_operation *op = pa_context_get_sink_info_by_name(volume_icon->ctx, volume_icon->sink,
                                                        sink_info, volume_icon);
    if (op != nullptr)
        pa_operation_unref(op);
}

static void flyout_redraw(volume *volume_icon);

static void first_sink([[maybe_unused]] pa_context *context, const pa_sink_info *info, int eol,
                       void *data) {
    volume *volume_icon = data;
    if (eol != 0 || info == nullptr || volume_icon->ready)
        return;
    g_strlcpy(volume_icon->sink, info->name, sizeof(volume_icon->sink));
    sink_info(context, info, 0, volume_icon);
}

static void sink_info([[maybe_unused]] pa_context *context, const pa_sink_info *info, int eol,
                      void *data) {
    volume *volume_icon = data;
    if (eol < 0) {
        /* No usable default output (no session manager to pick one):
         * the first output will do. */
        volume_icon->ready = false;
        pa_operation *op = pa_context_get_sink_info_list(volume_icon->ctx, first_sink, volume_icon);
        if (op != nullptr)
            pa_operation_unref(op);
        return;
    }
    if (eol != 0 || info == nullptr)
        return;
    /* Every change to any output lands here (streams starting and
     * stopping): redraw only for a new volume or mute. */
    if (volume_icon->ready && pa_cvolume_equal(&volume_icon->cv, &info->volume) &&
        volume_icon->muted == (info->mute != 0))
        return;
    volume_icon->cv = info->volume;
    volume_icon->muted = info->mute != 0;
    volume_icon->ready = true;
    xd_tray_changed(volume_icon->shell);
    flyout_redraw(volume_icon);
}

static void query(volume *volume_icon) {
    pa_operation *op = pa_context_get_server_info(volume_icon->ctx, server_info, volume_icon);
    if (op != nullptr)
        pa_operation_unref(op);
}

static void subscribed([[maybe_unused]] pa_context *context,
                       [[maybe_unused]] pa_subscription_event_type_t event_type,
                       [[maybe_unused]] uint32_t index, void *data) {
    query(data); /* the default output or its volume changed */
}

static gboolean retry_connect(gpointer data) {
    volume *volume_icon = data;
    volume_icon->retry = 0;
    connect_server(volume_icon);
    return G_SOURCE_REMOVE;
}

static void state_changed(pa_context *context, void *data) {
    volume *volume_icon = data;
    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY: {
        pa_context_set_subscribe_callback(context, subscribed, volume_icon);
        pa_operation *op = pa_context_subscribe(
            context,
            (pa_subscription_mask_t)(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER),
            nullptr, nullptr);
        if (op != nullptr)
            pa_operation_unref(op);
        query(volume_icon);
        break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        /* The sound server went away: try again in a while. */
        volume_icon->ready = false;
        xd_tray_changed(volume_icon->shell);
        pa_context_unref(volume_icon->ctx);
        volume_icon->ctx = nullptr;
        if (volume_icon->retry == 0)
            volume_icon->retry = g_timeout_add_seconds(5, retry_connect, volume_icon);
        break;
    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
        break;
    }
}

static void connect_server(volume *volume_icon) {
    volume_icon->ctx = pa_context_new(pa_glib_mainloop_get_api(volume_icon->loop), "XeonDesktop");
    if (volume_icon->ctx == nullptr)
        return;
    pa_context_set_state_callback(volume_icon->ctx, state_changed, volume_icon);
    if (pa_context_connect(volume_icon->ctx, nullptr, PA_CONTEXT_NOFAIL, nullptr) < 0)
        state_changed(volume_icon->ctx, volume_icon);
}

static void set_percent(volume *volume_icon, int pct) {
    if (!volume_icon->ready || volume_icon->ctx == nullptr)
        return;
    pct = XD_CLAMP(pct, 0, 100);
    pa_cvolume cv = volume_icon->cv;
    /* Keeps the balance between channels, as XP's slider does. */
    pa_cvolume_scale(&cv, (pa_volume_t)lround(pct * (double)PA_VOLUME_NORM / 100.0));
    volume_icon->cv = cv;
    pa_operation *op = pa_context_set_sink_volume_by_name(volume_icon->ctx, volume_icon->sink, &cv,
                                                          nullptr, nullptr);
    if (op != nullptr)
        pa_operation_unref(op);
    if (volume_icon->muted && pct > 0) {
        op = pa_context_set_sink_mute_by_name(volume_icon->ctx, volume_icon->sink, 0, nullptr,
                                              nullptr);
        if (op != nullptr)
            pa_operation_unref(op);
    }
    xd_tray_changed(volume_icon->shell);
    flyout_redraw(volume_icon);
}

static void set_mute(volume *volume_icon, bool mute) {
    if (!volume_icon->ready || volume_icon->ctx == nullptr)
        return;
    volume_icon->muted = mute;
    pa_operation *op = pa_context_set_sink_mute_by_name(volume_icon->ctx, volume_icon->sink, mute,
                                                        nullptr, nullptr);
    if (op != nullptr)
        pa_operation_unref(op);
    xd_tray_changed(volume_icon->shell);
    flyout_redraw(volume_icon);
}

/* ---- the flyout ------------------------------------------------------------------ */

/* Logical px. */
static constexpr int FLY_W = 72;
static constexpr int FLY_H = 158;
static constexpr int TRACK_TOP = 32;
static constexpr int TRACK_BOTTOM = 112;
static constexpr int THUMB_W = 22;
static constexpr int THUMB_H = 11;
/* The slider's keys, in percent: Windows' trackbar moves a line per arrow
 * and a fifth of its range per page. */
static constexpr int LINE_STEP = 2;
static constexpr int PAGE_STEP = 20;

struct flyout {
    volume *vol;
    xd_taskbar *bar; /* has the keyboard while we're open */
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
    double fraction = (TRACK_BOTTOM - y) / (double)(TRACK_BOTTOM - TRACK_TOP);
    return (int)lround(XD_CLAMP(fraction, 0.0, 1.0) * 100.0);
}

static void flyout_render(flyout *flyout_window) {
    xd_shell *shell = flyout_window->vol->shell;
    double scale = flyout_window->scale;
    double dwd = ceil(FLY_W * scale), dhd = ceil(FLY_H * scale);
    int dw = (int)dwd, dh = (int)dhd;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&flyout_window->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme theme;
    xd_theme_init(&theme, shell->style, &shell->scheme, scale);
    cairo_t *cairo = cairo_create(buf->surface);
    xd_rect all = {0, 0, dw, dh};
    xd_fill_rect(cairo, all, xd_sys(&theme, XD_COLOR_BUTTON_FACE));
    int px = xd_tpx(&theme, 1);
    /* A raised edge, like a small dialog. */
    xd_rect inner = xd_draw_bevel(cairo, all, px, xd_sys(&theme, XD_COLOR_BUTTON_LIGHT),
                                  xd_sys(&theme, XD_COLOR_BUTTON_DK_SHADOW));
    xd_draw_bevel(cairo, inner, px, xd_sys(&theme, XD_COLOR_BUTTON_HILIGHT),
                  xd_sys(&theme, XD_COLOR_BUTTON_SHADOW));

    xd_text_style title =
        xd_text(&theme, XD_FONT_MENU, xd_sys(&theme, XD_COLOR_BUTTON_TEXT), XD_ALIGN_CENTER);
    xd_draw_text(cairo, (xd_rect){0, xd_tpx(&theme, 6), dw, xd_tpx(&theme, 16)}, "Volume", &title);

    /* The track: a sunken groove with tick marks at the left. */
    int cx = dw / 2;
    int top = xd_tpx(&theme, TRACK_TOP), bottom = xd_tpx(&theme, TRACK_BOTTOM);
    xd_rect groove = {cx - 2 * px, top - 2 * px, 4 * px, bottom - top + 4 * px};
    xd_rect g2 = xd_draw_bevel(cairo, groove, px, xd_sys(&theme, XD_COLOR_BUTTON_SHADOW),
                               xd_sys(&theme, XD_COLOR_BUTTON_HILIGHT));
    xd_draw_bevel(cairo, g2, px, xd_sys(&theme, XD_COLOR_BUTTON_DK_SHADOW),
                  xd_sys(&theme, XD_COLOR_BUTTON_LIGHT));
    for (int i = 0; i <= 10; i++) {
        int ty = top + (bottom - top) * i / 10;
        xd_fill_rect(cairo, (xd_rect){cx - xd_tpx(&theme, 17), ty, xd_tpx(&theme, 4), px},
                     xd_sys(&theme, XD_COLOR_BUTTON_TEXT));
    }
    int pct = percent(flyout_window->vol);
    int thumb_y = bottom - (bottom - top) * pct / 100;
    xd_rect thumb = {cx - xd_tpx(&theme, THUMB_W) / 2, thumb_y - xd_tpx(&theme, THUMB_H) / 2,
                     xd_tpx(&theme, THUMB_W), xd_tpx(&theme, THUMB_H)};
    if (theme.style == XD_STYLE_CLASSIC) {
        xd_fill_rect(cairo, thumb, xd_sys(&theme, XD_COLOR_BUTTON_FACE));
        xd_rect ti = xd_draw_bevel(cairo, thumb, px, xd_sys(&theme, XD_COLOR_BUTTON_HILIGHT),
                                   xd_sys(&theme, XD_COLOR_BUTTON_DK_SHADOW));
        xd_draw_bevel(cairo, ti, px, xd_sys(&theme, XD_COLOR_BUTTON_LIGHT),
                      xd_sys(&theme, XD_COLOR_BUTTON_SHADOW));
    } else {
        /* XP's trackbar thumb: light, with a blue-grey outline. */
        xd_fill_rect(cairo, thumb, xd_rgba_hex(0xF3F3EE));
        xd_draw_bevel(cairo, thumb, px, xd_rgba_hex(0x7F9DB9), xd_rgba_hex(0x7F9DB9));
        xd_fill_rect(cairo,
                     (xd_rect){thumb.x + px, thumb.y + thumb.h - 2 * px, thumb.w - 2 * px, px},
                     xd_rgba_hex(0x21A121));
    }

    xd_rect rect = mute_rect();
    xd_rect mr = {xd_tpx(&theme, rect.x), xd_tpx(&theme, rect.y), xd_tpx(&theme, rect.w),
                  xd_tpx(&theme, rect.h)};
    xd_theme_draw_checkbox(&theme, cairo, mr, flyout_window->vol->muted ? XD_STATE_CHECKED : 0,
                           "Mute");
    cairo_destroy(cairo);
    cairo_surface_flush(buf->surface);
    wl_surface_attach(flyout_window->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(flyout_window->surface, 0, 0, dw, dh);
    if (flyout_window->viewport != nullptr)
        wp_viewport_set_destination(flyout_window->viewport, FLY_W, FLY_H);
    wl_surface_commit(flyout_window->surface);
    buf->busy = true;
    flyout_window->dirty = false;
}

static void flyout_close(volume *volume_icon) {
    flyout *flyout_window = volume_icon->fly;
    if (flyout_window == nullptr)
        return;
    volume_icon->fly = nullptr;
    volume_icon->closed_ms = g_get_monotonic_time() / 1000;
    xdg_popup_destroy(flyout_window->popup);
    xdg_surface_destroy(flyout_window->xdg_surface);
    if (flyout_window->viewport != nullptr)
        wp_viewport_destroy(flyout_window->viewport);
    wl_surface_destroy(flyout_window->surface);
    xd_shm_ring_finish(&flyout_window->ring);
    /* The keyboard back to the windows, unless the taskbar went with its
     * monitor meanwhile. */
    XD_LIST_FOR_EACH(bar, &volume_icon->shell->taskbars, xd_taskbar, link) {
        if (bar == flyout_window->bar && bar->layer != nullptr) {
            zwlr_layer_surface_v1_set_keyboard_interactivity(
                bar->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
            wl_surface_commit(bar->surface);
            bar->dirty = true;
        }
    }
    xd_free(flyout_window);
    xd_taskbars_schedule_tuck(volume_icon->shell);
}

static void flyout_redraw(volume *volume_icon) {
    if (volume_icon->fly != nullptr)
        volume_icon->fly->dirty = true;
}

static void fly_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    flyout *flyout_window = data;
    xdg_surface_ack_configure(surface, serial);
    flyout_window->configured = true;
    flyout_window->dirty = true;
}

static const struct xdg_surface_listener fly_surface_listener = {.configure = fly_configure};

static void fly_popup_configure([[maybe_unused]] void *data,
                                [[maybe_unused]] struct xdg_popup *popup,
                                [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                                [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height) {}

static void fly_popup_done(void *data, [[maybe_unused]] struct xdg_popup *popup) {
    flyout *flyout_window = data;
    flyout_close(flyout_window->vol); /* a click elsewhere */
}

static void fly_repositioned([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *popup,
                             [[maybe_unused]] uint32_t token) {}

static const struct xdg_popup_listener fly_popup_listener = {
    .configure = fly_popup_configure,
    .popup_done = fly_popup_done,
    .repositioned = fly_repositioned,
};

static void flyout_open(volume *volume_icon, xd_taskbar *bar, xd_rect anchor, uint32_t serial) {
    xd_shell *shell = volume_icon->shell;
    xd_menu_close_all(shell);
    /* A second click on the icon closes it: the press already did. */
    if (volume_icon->fly != nullptr) {
        flyout_close(volume_icon);
        return;
    }
    if (g_get_monotonic_time() / 1000 - volume_icon->closed_ms < 250)
        return;
    flyout *flyout_window = XD_NEW(flyout);
    flyout_window->vol = volume_icon;
    flyout_window->bar = bar;
    flyout_window->scale = bar->scale;
    flyout_window->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->viewporter != nullptr)
        flyout_window->viewport =
            wp_viewporter_get_viewport(shell->viewporter, flyout_window->surface);
    else
        flyout_window->scale = 1.0;
    flyout_window->xdg_surface =
        xdg_wm_base_get_xdg_surface(shell->wm_base, flyout_window->surface);
    xdg_surface_add_listener(flyout_window->xdg_surface, &fly_surface_listener, flyout_window);
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, FLY_W, FLY_H);
    xdg_positioner_set_anchor_rect(pos, anchor.x, anchor.y, XD_MAX(anchor.w, 1),
                                   XD_MAX(anchor.h, 1));
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_TOP);
    xdg_positioner_set_constraint_adjustment(pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                                      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
    flyout_window->popup = xdg_surface_get_popup(flyout_window->xdg_surface, nullptr, pos);
    xdg_positioner_destroy(pos);
    zwlr_layer_surface_v1_get_popup(bar->layer, flyout_window->popup);
    xdg_popup_add_listener(flyout_window->popup, &fly_popup_listener, flyout_window);
    /* The slider's keys reach us only with the keyboard taken before the
     * grab starts (see xd_taskbar_take_keyboard). */
    xd_taskbar_take_keyboard(bar);
    if (shell->seat != nullptr && serial != 0)
        xdg_popup_grab(flyout_window->popup, shell->seat, serial);
    wl_surface_commit(flyout_window->surface);
    volume_icon->fly = flyout_window;
}

bool xd_volume_flyout_owns(xd_shell *shell, struct wl_surface *surface) {
    (void)shell;
    return the_volume != nullptr && the_volume->fly != nullptr &&
           the_volume->fly->surface == surface;
}

void xd_volume_flyout_motion([[maybe_unused]] xd_shell *shell, [[maybe_unused]] double x,
                             double y) {
    volume *volume_icon = the_volume;
    if (volume_icon != nullptr && volume_icon->fly != nullptr && volume_icon->fly->dragging)
        set_percent(volume_icon, value_at(y));
}

void xd_volume_flyout_button([[maybe_unused]] xd_shell *shell, uint32_t button, bool pressed,
                             double x, double y) {
    volume *volume_icon = the_volume;
    if (volume_icon == nullptr || volume_icon->fly == nullptr || button != BTN_LEFT)
        return;
    flyout *flyout_window = volume_icon->fly;
    if (!pressed) {
        flyout_window->dragging = false;
        return;
    }
    xd_rect rect = mute_rect();
    if (x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h) {
        set_mute(volume_icon, !volume_icon->muted);
    } else if (y >= TRACK_TOP - THUMB_H && y <= TRACK_BOTTOM + THUMB_H) {
        flyout_window->dragging = true;
        set_percent(volume_icon, value_at(y));
    }
}

/* XP's flyout had the slider focused: a vertical trackbar, top loudest, so
 * Up, Left, Page Up and Home go up and Down, Right, Page Down and End go
 * down, as Windows' trackbar keys do. Space and Alt+M (its mnemonic)
 * check Mute; Esc closes it. */
bool xd_volume_flyout_key([[maybe_unused]] xd_shell *shell, uint32_t sym) {
    volume *volume_icon = the_volume;
    if (volume_icon == nullptr || volume_icon->fly == nullptr)
        return false;
    int level = percent(volume_icon);
    switch (sym) {
    case XKB_KEY_Up:
    case XKB_KEY_Left:
    case XKB_KEY_KP_Up:
    case XKB_KEY_KP_Left:
        set_percent(volume_icon, level + LINE_STEP);
        break;
    case XKB_KEY_Down:
    case XKB_KEY_Right:
    case XKB_KEY_KP_Down:
    case XKB_KEY_KP_Right:
        set_percent(volume_icon, level - LINE_STEP);
        break;
    case XKB_KEY_Page_Up:
    case XKB_KEY_KP_Page_Up:
        set_percent(volume_icon, level + PAGE_STEP);
        break;
    case XKB_KEY_Page_Down:
    case XKB_KEY_KP_Page_Down:
        set_percent(volume_icon, level - PAGE_STEP);
        break;
    case XKB_KEY_Home:
    case XKB_KEY_KP_Home:
        set_percent(volume_icon, 100);
        break;
    case XKB_KEY_End:
    case XKB_KEY_KP_End:
        set_percent(volume_icon, 0);
        break;
    case XKB_KEY_space:
    case XKB_KEY_m:
    case XKB_KEY_M:
        set_mute(volume_icon, !volume_icon->muted);
        break;
    case XKB_KEY_Escape:
        flyout_close(volume_icon);
        break;
    default:
        break; /* others are the flyout's too while it's open */
    }
    return true;
}

void xd_volume_flyout_dismiss([[maybe_unused]] xd_shell *shell) {
    if (the_volume != nullptr)
        flyout_close(the_volume);
}

void xd_volume_flyout_render_dirty([[maybe_unused]] xd_shell *shell) {
    volume *volume_icon = the_volume;
    if (volume_icon != nullptr && volume_icon->fly != nullptr && volume_icon->fly->configured &&
        volume_icon->fly->dirty)
        flyout_render(volume_icon->fly);
}

/* ---- the icon ---------------------------------------------------------------------- */

static const char *icon(void *data) {
    volume *volume_icon = data;
    /* No sound server or output, or turned off in Sounds and Audio Devices. */
    if (!volume_icon->ready || !volume_icon->shell->settings.volume_icon)
        return nullptr;
    int level = percent(volume_icon);
    return volume_icon->muted || level == 0 ? "audio-volume-muted"
           : level < 34                     ? "audio-volume-low"
           : level < 67                     ? "audio-volume-medium"
                                            : "audio-volume-high";
}

static void draw(void *data, const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    volume *volume_icon = data;
    int level = percent(volume_icon);
    int waves = level == 0 ? 0 : level < 34 ? 1 : level < 67 ? 2 : 3;
    xd_theme_draw_tray_icon(theme, cairo, rect, XD_TRAY_ICON_VOLUME, waves,
                            volume_icon->muted || level == 0 ? XD_TRAY_ICON_OFF : 0);
}

static char *tooltip(void *data) {
    volume *volume_icon = data;
    return volume_icon->muted ? g_strdup("Volume: muted")
                              : g_strdup_printf("Volume: %d%%", percent(volume_icon));
}

static void activate(void *data, xd_taskbar *bar, xd_rect anchor, uint32_t serial) {
    flyout_open(data, bar, anchor, serial);
}

static void scroll(void *data, double delta) {
    volume *volume_icon = data;
    set_percent(volume_icon, percent(volume_icon) - (int)lround(delta * 5)); /* wheel up: louder */
}

static void act_mixer(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_shell_launch(shell, "pavucontrol");
}

static void act_properties(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-sound", nullptr));
}

static void act_mute(xd_shell *shell, void *data) {
    (void)shell;
    volume *volume_icon = data;
    set_mute(volume_icon, !volume_icon->muted);
}

static void menu(void *data, xd_menu_tree *tree, xd_menu_model *menu_model) {
    volume *volume_icon = data;
    xd_menu_item *it =
        xd_menu_add(tree, menu_model, XD_MENU_ACTION, "Open Volume Control", nullptr);
    it->action = act_mixer;
    it->bold = true;
    it->disabled = !xd_shell_have_program("pavucontrol");
    it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, "Adjust Audio Properties", nullptr);
    it->action = act_properties;
    it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, "Mute", nullptr);
    it->action = act_mute;
    it->action_data = volume_icon;
    it->checked = volume_icon->muted;
}

static void destroy(void *data) {
    volume *volume_icon = data;
    flyout_close(volume_icon);
    if (volume_icon->retry != 0)
        g_source_remove(volume_icon->retry);
    if (volume_icon->ctx != nullptr) {
        pa_context_set_state_callback(volume_icon->ctx, nullptr, nullptr);
        pa_context_disconnect(volume_icon->ctx);
        pa_context_unref(volume_icon->ctx);
    }
    pa_glib_mainloop_free(volume_icon->loop);
    if (the_volume == volume_icon)
        the_volume = nullptr;
    g_free(volume_icon);
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
    volume *volume_icon = g_new0(volume, 1);
    volume_icon->shell = shell;
    volume_icon->loop = pa_glib_mainloop_new(nullptr);
    the_volume = volume_icon;
    xd_tray_add_sysicon(shell, "volume", &ops, volume_icon);
    connect_server(volume_icon);
}
