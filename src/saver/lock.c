/* SPDX-License-Identifier: MIT
 * xd-lock: Lock Computer and screen savers.
 *
 *   xd-lock                     lock now ("Computer Locked")
 *   xd-lock --saver [ID]        run a screen saver; password protected if
 *                               desktop.ini says so (or --lock)
 *   xd-lock --saver ID --no-lock    just the saver (Display Properties'
 *                                   Preview, unprotected savers)
 *
 * Locking uses ext-session-lock: the compositor shows nothing but our
 * surfaces until we unlock, and keeps the screen locked if we crash. As in
 * Windows 2000, a locked computer shows "Computer Locked"; Ctrl+Alt+Del
 * opens "Unlock Computer", which checks the password with PAM (service
 * "xeondesktop", falling back to "login") on a worker thread so the screen
 * keeps responding.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "../core/alloc.h"
#include "../core/wlist.h"
#include "../settings/settings.h"
#include "../ui/icons.h"
#include "../ui/protocols.h"
#include "../ui/shm.h"
#include "savers.h"

typedef enum screen : uint8_t {
    SCREEN_SAVER,    /* the animation */
    SCREEN_LOCKED,   /* "Computer Locked: press Ctrl-Alt-Del" */
    SCREEN_UNLOCK,   /* user name and password */
    SCREEN_CHECKING, /* PAM is busy */
    SCREEN_FAILED,   /* "could not log you on" over SCREEN_UNLOCK */
} screen;

typedef struct app app;

typedef struct output {
    struct wl_list link;
    app *app;
    uint32_t global;
    struct wl_output *wl_output;
    int int_scale; /* wl_output.scale, without fractional scaling */

    struct wl_surface *surface;
    struct ext_session_lock_surface_v1 *lock_surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *fractional;
    double frac_scale; /* 0 until the compositor says */
    xd_shm_ring ring;
    int width, height; /* logical */
    bool configured;
    bool dirty;
    struct wl_callback *frame;
    bool throttled; /* the frame came; waiting for frame_fd */
    struct timespec last_frame;
    xd_saver *saver;
    int saver_w, saver_h;
} output;

/* Button hit areas from the last render (device pixels, on the dialog
 * output). */
typedef enum hit : uint8_t { HIT_NONE, HIT_OK, HIT_CANCEL } hit;

struct app {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct ext_session_lock_manager_v1 *lock_manager;
    struct ext_session_lock_v1 *lock;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape;
    struct wl_list outputs;

    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;

    xd_settings settings;
    xd_scheme scheme;
    xd_icons *icons;

    bool locking; /* holding the session lock (vs. an overlay saver) */
    bool locked;  /* the compositor confirmed */
    bool running;
    int exit_code;
    char saver_id[64]; /* empty: no saver */
    screen screen;
    int idle_seconds;   /* back to the saver after this long (0: never) */
    int idle_fd;
    int frame_fd; /* holds savers to SAVER_FPS */
    uint32_t pointer_serial;
    bool pointer_seen;
    double pointer_x0, pointer_y0;
    output *pointer_output;
    double pointer_x, pointer_y;
    hit pressed;

    char user[256];
    char who[800]; /* "user@host (Full Name)" */
    char password[256];
    size_t password_len;

    /* PAM worker */
    int pam_pipe[2];
    pthread_t pam_thread;
    bool pam_running;
    char pam_password[256];

    xd_rect ok_rect, cancel_rect; /* device px on the dialog output */
    output *dialog_output;
};

/* ---- PAM -------------------------------------------------------------------- */

static int pam_conv_cb(int n, const struct pam_message **msg, struct pam_response **resp,
                       void *data) {
    const char *password = data;
    if (n <= 0 || n > PAM_MAX_NUM_MSG)
        return PAM_CONV_ERR;
    struct pam_response *r = calloc((size_t)n, sizeof(*r));
    if (r == nullptr)
        return PAM_BUF_ERR;
    for (int i = 0; i < n; i++) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            r[i].resp = strdup(password);
            if (r[i].resp == nullptr) {
                for (int j = 0; j < i; j++)
                    free(r[j].resp);
                free(r);
                return PAM_BUF_ERR;
            }
        }
    }
    *resp = r;
    return PAM_SUCCESS;
}

static const char *pam_service(void) {
    static const char *const services[] = {"xeondesktop", "login"};
    for (size_t i = 0; i < XD_ARRAY_LEN(services); i++) {
        char path[128];
        snprintf(path, sizeof(path), "/etc/pam.d/%s", services[i]);
        if (access(path, R_OK) == 0)
            return services[i];
    }
    return "login";
}

static void *pam_worker(void *data) {
    app *a = data;
    struct pam_conv conv = {.conv = pam_conv_cb, .appdata_ptr = a->pam_password};
    pam_handle_t *h = nullptr;
    int r = pam_start(pam_service(), a->user, &conv, &h);
    if (r == PAM_SUCCESS)
        r = pam_authenticate(h, 0);
    if (h != nullptr)
        pam_end(h, r);
    explicit_bzero(a->pam_password, sizeof(a->pam_password));
    char ok = r == PAM_SUCCESS ? 1 : 0;
    (void)!write(a->pam_pipe[1], &ok, 1);
    return nullptr;
}

static void start_check(app *a) {
    if (a->pam_running)
        return;
    memcpy(a->pam_password, a->password, sizeof(a->password));
    explicit_bzero(a->password, sizeof(a->password));
    a->password_len = 0;
    a->pam_running = true;
    a->screen = SCREEN_CHECKING;
    if (pthread_create(&a->pam_thread, nullptr, pam_worker, a) != 0) {
        a->pam_running = false;
        explicit_bzero(a->pam_password, sizeof(a->pam_password));
        a->screen = SCREEN_FAILED;
    }
}

/* ---- screens ------------------------------------------------------------------ */

static void redraw_all(app *a) {
    XD_LIST_FOR_EACH(o, &a->outputs, output, link) {
        o->dirty = true;
    }
}

static void set_cursor(app *a) {
    if (a->pointer == nullptr)
        return;
    if (a->screen == SCREEN_SAVER)
        wl_pointer_set_cursor(a->pointer, a->pointer_serial, nullptr, 0, 0);
    else if (a->cursor_shape != nullptr)
        wp_cursor_shape_device_v1_set_shape(a->cursor_shape, a->pointer_serial,
                                            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

static void arm_idle(app *a) {
    struct itimerspec spec = {0};
    if (a->idle_seconds > 0 && a->screen != SCREEN_SAVER && a->saver_id[0] != '\0')
        spec.it_value.tv_sec = a->idle_seconds;
    timerfd_settime(a->idle_fd, 0, &spec, nullptr);
}

static void go(app *a, screen s) {
    if (s != SCREEN_UNLOCK && s != SCREEN_FAILED && s != SCREEN_CHECKING) {
        explicit_bzero(a->password, sizeof(a->password));
        a->password_len = 0;
    }
    a->screen = s;
    /* Pointer movement ends a saver only beyond a small distance from
     * where the pointer is now. */
    a->pointer_seen = a->pointer_output != nullptr;
    a->pointer_x0 = a->pointer_x;
    a->pointer_y0 = a->pointer_y;
    a->pressed = HIT_NONE;
    set_cursor(a);
    arm_idle(a);
    redraw_all(a);
}

static void unlock(app *a) {
    if (a->lock != nullptr) {
        if (a->locked)
            ext_session_lock_v1_unlock_and_destroy(a->lock);
        else
            ext_session_lock_v1_destroy(a->lock);
        a->lock = nullptr;
        wl_display_roundtrip(a->display);
    }
    a->running = false;
}

/* Any input while the saver runs. */
static void saver_input(app *a) {
    if (!a->locking)
        a->running = false;
    else
        go(a, SCREEN_LOCKED);
}

static void check_result(app *a) {
    char ok = 0;
    (void)!read(a->pam_pipe[0], &ok, 1);
    if (a->pam_running) {
        pthread_join(a->pam_thread, nullptr);
        a->pam_running = false;
    }
    if (ok)
        unlock(a);
    else
        go(a, SCREEN_FAILED);
}

/* ---- drawing ------------------------------------------------------------------ */

static double output_scale(const output *o) {
    return o->frac_scale > 0 ? o->frac_scale : (double)XD_MAX(o->int_scale, 1);
}

static void draw_icon(app *a, cairo_t *cr, const char *name, int x, int y, int px) {
    static const char *const fallback[] = {"system-lock-screen", "dialog-password",
                                           "dialog-information"};
    cairo_surface_t *icon = xd_icons_get(a->icons, name, px);
    if (icon == nullptr)
        icon = xd_icons_get_any(a->icons, fallback, XD_ARRAY_LEN(fallback), px);
    if (icon != nullptr) {
        cairo_set_source_surface(cr, icon, x, y);
        cairo_paint(cr);
    }
}

/* Logical -> device rect relative to an origin. */
static xd_rect L(const xd_theme *t, xd_rect origin, int x, int y, int w, int h) {
    return (xd_rect){origin.x + xd_tpx(t, x), origin.y + xd_tpx(t, y), xd_tpx(t, w),
                     xd_tpx(t, h)};
}

/* A centred dialog with a frame; returns its client area. */
static xd_rect dialog(const xd_theme *t, cairo_t *cr, int dev_w, int dev_h, const char *title,
                      int client_w, int client_h) {
    xd_frame f = {.title = title, .active = true, .dialog = true};
    xd_insets in = xd_theme_frame_insets(t, &f);
    int w = xd_tpx(t, client_w) + in.left + in.right;
    int h = xd_tpx(t, client_h) + in.top + in.bottom;
    xd_rect outer = {(dev_w - w) / 2, (dev_h - h) / 2, w, h};
    xd_theme_draw_frame(t, cr, outer, &f);
    xd_rect client = xd_rect_inset4(outer, in.left, in.top, in.right, in.bottom);
    xd_fill_rect(cr, client, xd_theme_dialog_color(t));
    return client;
}

static void draw_text(const xd_theme *t, cairo_t *cr, xd_rect r, const char *text, bool wrap,
                      bool dim) {
    xd_text_style st = xd_text(t, XD_FONT_MESSAGE,
                               xd_sys(t, dim ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT),
                               XD_ALIGN_START);
    st.wrap = wrap;
    st.ellipsize = !wrap;
    xd_draw_text(cr, r, text, &st);
}

static void draw_field(const xd_theme *t, cairo_t *cr, xd_rect r, const char *text, bool disabled,
                       bool caret) {
    xd_rect in = xd_theme_draw_field(t, cr, r, disabled);
    xd_text_style st =
        xd_text(t, XD_FONT_MESSAGE, xd_sys(t, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_WINDOW_TEXT),
                XD_ALIGN_START);
    xd_rect tr = xd_rect_inset4(in, xd_tpx(t, 2), 0, xd_tpx(t, 2), 0);
    xd_draw_text(cr, tr, text, &st);
    if (caret) {
        int x = tr.x + xd_text_width(cr, text, &st) + xd_tpx(t, 1);
        xd_fill_rect(cr, (xd_rect){x, in.y + xd_tpx(t, 2), xd_tpx(t, 1), in.h - xd_tpx(t, 4)},
                     xd_sys(t, XD_COLOR_WINDOW_TEXT));
    }
}

static bool caps_lock(const app *a) {
    return a->xkb_state != nullptr &&
           xkb_state_mod_name_is_active(a->xkb_state, XKB_MOD_NAME_CAPS,
                                        XKB_STATE_MODS_LOCKED) > 0;
}

static void draw_lock_dialog(app *a, const xd_theme *t, cairo_t *cr, int dw, int dh) {
    bool unlock_form = a->screen != SCREEN_LOCKED;
    enum { W = 412 };
    int client_h = unlock_form ? 268 : 196;
    xd_rect c = dialog(t, cr, dw, dh, unlock_form ? "Unlock Computer" : "Computer Locked", W,
                       client_h);
    xd_theme_draw_dialog_banner(t, cr, L(t, c, 0, 0, W, 72), "XeonDesktop", "Professional");
    draw_icon(a, cr, "system-lock-screen", c.x + xd_tpx(t, 14), c.y + xd_tpx(t, 86),
              xd_tpx(t, 32));
    draw_text(t, cr, L(t, c, 60, 86, W - 74, 16),
              "This computer is in use and has been locked.", false, false);
    char only[900];
    snprintf(only, sizeof(only), "Only %s or an administrator can unlock this computer.", a->who);
    draw_text(t, cr, L(t, c, 60, 110, W - 74, 32), only, true, false);

    if (!unlock_form) {
        draw_text(t, cr, L(t, c, 60, 154, W - 74, 16),
                  "Press Ctrl-Alt-Del to unlock this computer.", false, false);
        a->ok_rect = a->cancel_rect = (xd_rect){0};
        return;
    }
    bool busy = a->screen == SCREEN_CHECKING;
    draw_text(t, cr, L(t, c, 60, 157, 76, 16), "User name:", false, busy);
    draw_field(t, cr, L(t, c, 140, 154, W - 154, 21), a->user, true, false);
    draw_text(t, cr, L(t, c, 60, 185, 76, 16), "Password:", false, busy);
    char stars[sizeof(a->password) * 3 + 1];
    size_t n = 0;
    const char *bullet = t->style == XD_STYLE_CLASSIC ? "*" : "●";
    for (size_t i = 0; i < a->password_len && n + 4 < sizeof(stars); i++) {
        size_t bl = strlen(bullet);
        memcpy(stars + n, bullet, bl);
        n += bl;
    }
    stars[n] = '\0';
    draw_field(t, cr, L(t, c, 140, 182, W - 154, 21), stars, busy,
               a->screen == SCREEN_UNLOCK);
    if (busy)
        draw_text(t, cr, L(t, c, 140, 207, W - 154, 16), "Checking your password...", false,
                  true);
    else if (caps_lock(a))
        draw_text(t, cr, L(t, c, 140, 207, W - 154, 16), "Caps Lock is on.", false, true);

    a->ok_rect = L(t, c, W - 170, 234, 75, 23);
    a->cancel_rect = L(t, c, W - 89, 234, 75, 23);
    uint8_t dis = busy || a->screen == SCREEN_FAILED ? XD_STATE_DISABLED : 0;
    xd_theme_draw_button(t, cr, a->ok_rect,
                         (uint8_t)(dis | XD_STATE_DEFAULT |
                                   (a->pressed == HIT_OK ? XD_STATE_PRESSED : 0)),
                         "OK");
    xd_theme_draw_button(t, cr, a->cancel_rect,
                         (uint8_t)(dis | (a->pressed == HIT_CANCEL ? XD_STATE_PRESSED : 0)),
                         "Cancel");

    if (a->screen == SCREEN_FAILED) {
        enum { MW = 340 };
        xd_rect m = dialog(t, cr, dw, dh, "Unlock Computer", MW, 112);
        draw_icon(a, cr, "dialog-error", m.x + xd_tpx(t, 12), m.y + xd_tpx(t, 14),
                  xd_tpx(t, 32));
        draw_text(t, cr, L(t, m, 56, 14, MW - 68, 56),
                  "The system could not log you on. Make sure your password is correct, then "
                  "type it again. Letters in passwords must be typed using the correct case.",
                  true, false);
        a->ok_rect = L(t, m, (MW - 75) / 2, 78, 75, 23);
        a->cancel_rect = (xd_rect){0};
        xd_theme_draw_button(t, cr, a->ok_rect,
                             (uint8_t)(XD_STATE_DEFAULT |
                                       (a->pressed == HIT_OK ? XD_STATE_PRESSED : 0)),
                             "OK");
    }
}

/* A saver runs for hours: drawing it at a 144 Hz monitor's rate would
 * only burn CPU, so frames come no faster than this. */
static constexpr double SAVER_FPS = 30.0;

static double elapsed(const struct timespec *last, const struct timespec *now) {
    return last->tv_sec == 0 ? 0.0
                             : (double)(now->tv_sec - last->tv_sec) +
                                   (double)(now->tv_nsec - last->tv_nsec) / 1e9;
}

static double seconds_since(struct timespec *last) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = elapsed(last, &now);
    *last = now;
    return dt;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void frame_done(void *data, struct wl_callback *cb, [[maybe_unused]] uint32_t time) {
    output *o = data;
    app *a = o->app;
    wl_callback_destroy(cb);
    o->frame = nullptr;
    if (a->screen != SCREEN_SAVER)
        return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double wait = 1.0 / SAVER_FPS - elapsed(&o->last_frame, &now);
    if (wait <= 0.002) {
        o->dirty = true;
        return;
    }
    o->throttled = true;
    struct itimerspec cur;
    timerfd_gettime(a->frame_fd, &cur);
    long ns = (long)(wait * 1e9);
    if ((cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0) ||
        (cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > ns)) {
        struct itimerspec spec = {.it_value = {.tv_nsec = ns}};
        timerfd_settime(a->frame_fd, 0, &spec, nullptr);
    }
}

static void render(output *o) {
    app *a = o->app;
    double s = output_scale(o);
    int dw = (int)lround(o->width * s);
    int dh = (int)lround(o->height * s);
    if (dw <= 0 || dh <= 0)
        return;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&o->ring, a->shm, dw, dh);
    if (buf == nullptr)
        return; /* drawn when a buffer comes back */
    cairo_t *cr = cairo_create(buf->surface);
    xd_theme t;
    xd_theme_init(&t, a->settings.style, &a->scheme, s);

    if (a->screen == SCREEN_SAVER) {
        if (o->saver == nullptr || o->saver_w != dw || o->saver_h != dh) {
            if (o->saver != nullptr)
                xd_saver_destroy(o->saver);
            o->saver = xd_saver_create(a->saver_id, dw, dh, s);
            o->saver_w = dw;
            o->saver_h = dh;
            o->last_frame = (struct timespec){0};
        }
        double dt = seconds_since(&o->last_frame);
        if (o->saver != nullptr)
            xd_saver_frame(o->saver, cr, dt);
        else
            xd_fill_rect(cr, (xd_rect){0, 0, dw, dh}, xd_rgba_hex(0x000000));
    } else {
        /* The logon desktop: the scheme's background colour, no wallpaper. */
        xd_fill_rect(cr, (xd_rect){0, 0, dw, dh}, xd_sys(&t, XD_COLOR_BACKGROUND));
        if (o == a->dialog_output)
            draw_lock_dialog(a, &t, cr, dw, dh);
    }
    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);

    /* A still saver (Blank Screen) is drawn once, not every frame. */
    if (a->screen == SCREEN_SAVER && o->frame == nullptr && xd_saver_animated(a->saver_id)) {
        o->frame = wl_surface_frame(o->surface);
        wl_callback_add_listener(o->frame, &frame_listener, o);
    }
    wl_surface_attach(o->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(o->surface, 0, 0, dw, dh);
    if (o->viewport != nullptr)
        wp_viewport_set_destination(o->viewport, o->width, o->height);
    else
        wl_surface_set_buffer_scale(o->surface, XD_MAX(o->int_scale, 1));
    wl_surface_commit(o->surface);
    buf->busy = true;
    o->dirty = false;
}

/* ---- surfaces ----------------------------------------------------------------- */

static void configured(output *o, int w, int h) {
    o->width = w;
    o->height = h;
    o->configured = true;
    o->dirty = true;
    app *a = o->app;
    if (a->dialog_output == nullptr)
        a->dialog_output = o;
}

static void lock_surface_configure(void *data, struct ext_session_lock_surface_v1 *ls,
                                   uint32_t serial, uint32_t w, uint32_t h) {
    ext_session_lock_surface_v1_ack_configure(ls, serial);
    configured(data, (int)w, (int)h);
}

static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
    .configure = lock_surface_configure,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
                            uint32_t w, uint32_t h) {
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    configured(data, (int)w, (int)h);
}

static void layer_closed(void *data, [[maybe_unused]] struct zwlr_layer_surface_v1 *ls) {
    output *o = data;
    o->app->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void fractional_scale(void *data, [[maybe_unused]] struct wp_fractional_scale_v1 *f,
                             uint32_t scale) {
    output *o = data;
    o->frac_scale = scale / 120.0;
    o->dirty = true;
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = fractional_scale,
};

static void create_surface(app *a, output *o) {
    if (o->surface != nullptr || a->compositor == nullptr)
        return;
    if (a->locking && a->lock == nullptr)
        return;
    o->surface = wl_compositor_create_surface(a->compositor);
    if (a->viewporter != nullptr && a->fractional_manager != nullptr) {
        o->viewport = wp_viewporter_get_viewport(a->viewporter, o->surface);
        o->fractional =
            wp_fractional_scale_manager_v1_get_fractional_scale(a->fractional_manager, o->surface);
        wp_fractional_scale_v1_add_listener(o->fractional, &fractional_listener, o);
    }
    if (a->locking) {
        o->lock_surface = ext_session_lock_v1_get_lock_surface(a->lock, o->surface, o->wl_output);
        ext_session_lock_surface_v1_add_listener(o->lock_surface, &lock_surface_listener, o);
    } else {
        o->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            a->layer_shell, o->surface, o->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
            "screensaver");
        zwlr_layer_surface_v1_set_anchor(o->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                               ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                               ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                               ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(o->layer_surface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            o->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
        zwlr_layer_surface_v1_add_listener(o->layer_surface, &layer_listener, o);
        wl_surface_commit(o->surface);
    }
}

static void destroy_output(output *o) {
    app *a = o->app;
    if (a->dialog_output == o) {
        a->dialog_output = nullptr;
        XD_LIST_FOR_EACH(other, &a->outputs, output, link) {
            if (other != o && other->configured && a->dialog_output == nullptr) {
                a->dialog_output = other;
                other->dirty = true;
            }
        }
    }
    if (a->pointer_output == o)
        a->pointer_output = nullptr;
    if (o->frame != nullptr)
        wl_callback_destroy(o->frame);
    if (o->saver != nullptr)
        xd_saver_destroy(o->saver);
    if (o->lock_surface != nullptr)
        ext_session_lock_surface_v1_destroy(o->lock_surface);
    if (o->layer_surface != nullptr)
        zwlr_layer_surface_v1_destroy(o->layer_surface);
    if (o->fractional != nullptr)
        wp_fractional_scale_v1_destroy(o->fractional);
    if (o->viewport != nullptr)
        wp_viewport_destroy(o->viewport);
    if (o->surface != nullptr)
        wl_surface_destroy(o->surface);
    xd_shm_ring_finish(&o->ring);
    wl_output_release(o->wl_output);
    wl_list_remove(&o->link);
    xd_free(o);
}

/* ---- session lock ---------------------------------------------------------------- */

static void lock_locked(void *data, [[maybe_unused]] struct ext_session_lock_v1 *l) {
    app *a = data;
    a->locked = true;
}

static void lock_finished(void *data, [[maybe_unused]] struct ext_session_lock_v1 *l) {
    app *a = data;
    if (!a->locked)
        fprintf(stderr, "xd-lock: the compositor refused to lock (another locker running?)\n");
    else
        fprintf(stderr, "xd-lock: the compositor ended the lock\n");
    a->exit_code = a->locked ? EXIT_SUCCESS : EXIT_FAILURE;
    a->running = false;
}

static const struct ext_session_lock_v1_listener lock_listener = {
    .locked = lock_locked,
    .finished = lock_finished,
};

/* ---- input -------------------------------------------------------------------- */

static void user_activity(app *a) { arm_idle(a); }

static hit hit_at(const app *a, const output *o, double x, double y) {
    if (o != a->dialog_output)
        return HIT_NONE;
    double s = output_scale(o);
    double dx = x * s;
    double dy = y * s;
    const xd_rect *rs[] = {&a->ok_rect, &a->cancel_rect};
    for (size_t i = 0; i < 2; i++) {
        const xd_rect *r = rs[i];
        if (r->w > 0 && dx >= r->x && dy >= r->y && dx < r->x + r->w && dy < r->y + r->h)
            return i == 0 ? HIT_OK : HIT_CANCEL;
    }
    return HIT_NONE;
}

static void press_ok(app *a) {
    if (a->screen == SCREEN_FAILED)
        go(a, SCREEN_UNLOCK);
    else if (a->screen == SCREEN_UNLOCK) {
        start_check(a);
        redraw_all(a);
    }
}

static void pointer_enter(void *data, [[maybe_unused]] struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    app *a = data;
    a->pointer_serial = serial;
    a->pointer_output = nullptr;
    XD_LIST_FOR_EACH(o, &a->outputs, output, link) {
        if (o->surface == surface)
            a->pointer_output = o;
    }
    a->pointer_x = wl_fixed_to_double(x);
    a->pointer_y = wl_fixed_to_double(y);
    if (!a->pointer_seen) {
        a->pointer_seen = true;
        a->pointer_x0 = a->pointer_x;
        a->pointer_y0 = a->pointer_y;
    }
    set_cursor(a);
}

static void pointer_leave(void *data, [[maybe_unused]] struct wl_pointer *p,
                          [[maybe_unused]] uint32_t serial,
                          [[maybe_unused]] struct wl_surface *surface) {
    app *a = data;
    a->pointer_output = nullptr;
}

static void pointer_motion(void *data, [[maybe_unused]] struct wl_pointer *p,
                           [[maybe_unused]] uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    app *a = data;
    a->pointer_x = wl_fixed_to_double(x);
    a->pointer_y = wl_fixed_to_double(y);
    if (a->screen != SCREEN_SAVER) {
        user_activity(a);
        return;
    }
    /* Like Windows, ignore a nudge: only a real move ends the saver. */
    if (!a->pointer_seen) {
        a->pointer_seen = true;
        a->pointer_x0 = a->pointer_x;
        a->pointer_y0 = a->pointer_y;
    } else if (fabs(a->pointer_x - a->pointer_x0) + fabs(a->pointer_y - a->pointer_y0) > 12) {
        saver_input(a);
    }
}

static void pointer_button(void *data, [[maybe_unused]] struct wl_pointer *p, uint32_t serial,
                           [[maybe_unused]] uint32_t time, [[maybe_unused]] uint32_t button,
                           uint32_t state) {
    app *a = data;
    a->pointer_serial = serial;
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (a->screen == SCREEN_SAVER) {
        if (pressed)
            saver_input(a);
        return;
    }
    user_activity(a);
    hit h = a->pointer_output != nullptr
                ? hit_at(a, a->pointer_output, a->pointer_x, a->pointer_y)
                : HIT_NONE;
    bool usable = a->screen == SCREEN_UNLOCK || a->screen == SCREEN_FAILED;
    if (pressed) {
        a->pressed = usable ? h : HIT_NONE;
        redraw_all(a);
        return;
    }
    hit was = a->pressed;
    a->pressed = HIT_NONE;
    redraw_all(a);
    if (was == HIT_NONE || was != h)
        return;
    if (was == HIT_OK)
        press_ok(a);
    else if (was == HIT_CANCEL && a->screen == SCREEN_UNLOCK)
        go(a, SCREEN_LOCKED);
}

static void pointer_axis(void *data, [[maybe_unused]] struct wl_pointer *p,
                         [[maybe_unused]] uint32_t time, [[maybe_unused]] uint32_t axis,
                         [[maybe_unused]] wl_fixed_t value) {
    app *a = data;
    if (a->screen == SCREEN_SAVER)
        saver_input(a);
}

static void pointer_frame([[maybe_unused]] void *data, [[maybe_unused]] struct wl_pointer *p) {}
static void pointer_axis_source([[maybe_unused]] void *data,
                                [[maybe_unused]] struct wl_pointer *p,
                                [[maybe_unused]] uint32_t source) {}
static void pointer_axis_stop([[maybe_unused]] void *data, [[maybe_unused]] struct wl_pointer *p,
                              [[maybe_unused]] uint32_t time, [[maybe_unused]] uint32_t axis) {}
static void pointer_axis_discrete([[maybe_unused]] void *data,
                                  [[maybe_unused]] struct wl_pointer *p,
                                  [[maybe_unused]] uint32_t axis,
                                  [[maybe_unused]] int32_t discrete) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void keyboard_keymap(void *data, [[maybe_unused]] struct wl_keyboard *k, uint32_t format,
                            int32_t fd, uint32_t size) {
    app *a = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0 || size > (16u << 20)) {
        close(fd);
        return;
    }
    char *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return;
    struct xkb_keymap *keymap = nullptr;
    if (memchr(map, '\0', size) != nullptr)
        keymap = xkb_keymap_new_from_string(a->xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (keymap == nullptr)
        return;
    xkb_state_unref(a->xkb_state);
    xkb_keymap_unref(a->keymap);
    a->keymap = keymap;
    a->xkb_state = xkb_state_new(keymap);
}

static void keyboard_enter([[maybe_unused]] void *data, [[maybe_unused]] struct wl_keyboard *k,
                           [[maybe_unused]] uint32_t serial,
                           [[maybe_unused]] struct wl_surface *surface,
                           [[maybe_unused]] struct wl_array *keys) {}
static void keyboard_leave([[maybe_unused]] void *data, [[maybe_unused]] struct wl_keyboard *k,
                           [[maybe_unused]] uint32_t serial,
                           [[maybe_unused]] struct wl_surface *surface) {}

static bool mod_active(const app *a, const char *name) {
    return xkb_state_mod_name_is_active(a->xkb_state, name, XKB_STATE_MODS_EFFECTIVE) > 0;
}

static void password_key(app *a, xkb_keysym_t sym, xkb_keycode_t code) {
    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        press_ok(a);
        return;
    case XKB_KEY_Escape:
        go(a, SCREEN_LOCKED);
        return;
    case XKB_KEY_BackSpace:
        if (mod_active(a, XKB_MOD_NAME_CTRL)) {
            explicit_bzero(a->password, sizeof(a->password));
            a->password_len = 0;
        } else if (a->password_len > 0) {
            /* Back over one UTF-8 character. */
            do {
                a->password[--a->password_len] = '\0';
            } while (a->password_len > 0 && (a->password[a->password_len] & 0xC0) == 0x80);
        }
        redraw_all(a);
        return;
    default:
        break;
    }
    char utf8[16] = {0};
    int n = xkb_state_key_get_utf8(a->xkb_state, code, utf8, sizeof(utf8));
    if (n <= 0 || (unsigned char)utf8[0] < 0x20 || utf8[0] == 0x7F)
        return;
    if (a->password_len + (size_t)n >= sizeof(a->password))
        return;
    memcpy(a->password + a->password_len, utf8, (size_t)n);
    a->password_len += (size_t)n;
    explicit_bzero(utf8, sizeof(utf8));
    redraw_all(a);
}

static void keyboard_key(void *data, [[maybe_unused]] struct wl_keyboard *k,
                         [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                         uint32_t key, uint32_t state) {
    app *a = data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || a->xkb_state == nullptr)
        return;
    xkb_keycode_t code = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(a->xkb_state, code);
    bool modifier = sym == XKB_KEY_Control_L || sym == XKB_KEY_Control_R ||
                    sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R || sym == XKB_KEY_Shift_L ||
                    sym == XKB_KEY_Shift_R || sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R;
    switch (a->screen) {
    case SCREEN_SAVER:
        saver_input(a);
        return;
    case SCREEN_LOCKED:
        user_activity(a);
        if (!modifier && (sym == XKB_KEY_Delete || sym == XKB_KEY_KP_Delete) &&
            mod_active(a, XKB_MOD_NAME_CTRL) && mod_active(a, XKB_MOD_NAME_ALT))
            go(a, SCREEN_UNLOCK);
        return;
    case SCREEN_UNLOCK:
        user_activity(a);
        password_key(a, sym, code);
        return;
    case SCREEN_FAILED:
        user_activity(a);
        if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_Escape ||
            sym == XKB_KEY_space)
            go(a, SCREEN_UNLOCK);
        return;
    case SCREEN_CHECKING:
        return;
    }
}

static void keyboard_modifiers(void *data, [[maybe_unused]] struct wl_keyboard *k,
                               [[maybe_unused]] uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group) {
    app *a = data;
    if (a->xkb_state == nullptr)
        return;
    bool caps = caps_lock(a);
    xkb_state_update_mask(a->xkb_state, depressed, latched, locked, 0, 0, group);
    if (caps != caps_lock(a))
        redraw_all(a); /* the Caps Lock warning */
}

static void keyboard_repeat_info([[maybe_unused]] void *data,
                                 [[maybe_unused]] struct wl_keyboard *k,
                                 [[maybe_unused]] int32_t rate, [[maybe_unused]] int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    app *a = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && a->pointer == nullptr) {
        a->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(a->pointer, &pointer_listener, a);
        if (a->cursor_shape_manager != nullptr)
            a->cursor_shape =
                wp_cursor_shape_manager_v1_get_pointer(a->cursor_shape_manager, a->pointer);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && a->keyboard == nullptr) {
        a->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(a->keyboard, &keyboard_listener, a);
    }
    /* A keyboard or mouse unplugged (the last one): our objects go dead,
     * so get new ones when one comes back. */
    if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && a->keyboard != nullptr) {
        wl_keyboard_release(a->keyboard);
        a->keyboard = nullptr;
    }
    if (!(caps & WL_SEAT_CAPABILITY_POINTER) && a->pointer != nullptr) {
        if (a->cursor_shape != nullptr) {
            wp_cursor_shape_device_v1_destroy(a->cursor_shape);
            a->cursor_shape = nullptr;
        }
        wl_pointer_release(a->pointer);
        a->pointer = nullptr;
        a->pointer_output = nullptr;
    }
}

static void seat_name([[maybe_unused]] void *data, [[maybe_unused]] struct wl_seat *seat,
                      [[maybe_unused]] const char *name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ---- outputs and globals --------------------------------------------------------- */

static void out_geometry([[maybe_unused]] void *data, [[maybe_unused]] struct wl_output *o,
                         [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                         [[maybe_unused]] int32_t pw, [[maybe_unused]] int32_t ph,
                         [[maybe_unused]] int32_t sub, [[maybe_unused]] const char *make,
                         [[maybe_unused]] const char *model, [[maybe_unused]] int32_t t) {}
static void out_mode([[maybe_unused]] void *data, [[maybe_unused]] struct wl_output *o,
                     [[maybe_unused]] uint32_t flags, [[maybe_unused]] int32_t w,
                     [[maybe_unused]] int32_t h, [[maybe_unused]] int32_t refresh) {}
static void out_done([[maybe_unused]] void *data, [[maybe_unused]] struct wl_output *o) {}
static void out_scale(void *data, [[maybe_unused]] struct wl_output *wo, int32_t factor) {
    output *o = data;
    o->int_scale = factor;
    o->dirty = true;
}
static void out_name([[maybe_unused]] void *data, [[maybe_unused]] struct wl_output *o,
                     [[maybe_unused]] const char *name) {}
static void out_description([[maybe_unused]] void *data, [[maybe_unused]] struct wl_output *o,
                            [[maybe_unused]] const char *d) {}

static const struct wl_output_listener output_listener = {
    .geometry = out_geometry,
    .mode = out_mode,
    .done = out_done,
    .scale = out_scale,
    .name = out_name,
    .description = out_description,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
    app *a = data;
#define IS(i) (strcmp(iface, (i).name) == 0)
#define BIND(i, v) wl_registry_bind(reg, name, &(i), XD_MIN(version, (uint32_t)(v)))
    if (IS(wl_compositor_interface)) {
        a->compositor = BIND(wl_compositor_interface, 4);
    } else if (IS(wl_shm_interface)) {
        a->shm = BIND(wl_shm_interface, 1);
    } else if (IS(wl_seat_interface) && a->seat == nullptr) {
        a->seat = BIND(wl_seat_interface, 5);
        wl_seat_add_listener(a->seat, &seat_listener, a);
    } else if (IS(ext_session_lock_manager_v1_interface)) {
        a->lock_manager = BIND(ext_session_lock_manager_v1_interface, 1);
    } else if (IS(zwlr_layer_shell_v1_interface)) {
        a->layer_shell = BIND(zwlr_layer_shell_v1_interface, 4);
    } else if (IS(wp_viewporter_interface)) {
        a->viewporter = BIND(wp_viewporter_interface, 1);
    } else if (IS(wp_fractional_scale_manager_v1_interface)) {
        a->fractional_manager = BIND(wp_fractional_scale_manager_v1_interface, 1);
    } else if (IS(wp_cursor_shape_manager_v1_interface)) {
        a->cursor_shape_manager = BIND(wp_cursor_shape_manager_v1_interface, 1);
    } else if (IS(wl_output_interface)) {
        output *o = XD_NEW(output);
        o->app = a;
        o->global = name;
        o->int_scale = 1;
        o->wl_output = BIND(wl_output_interface, 4);
        wl_output_add_listener(o->wl_output, &output_listener, o);
        wl_list_insert(a->outputs.prev, &o->link);
        /* A monitor plugged in while locked gets covered too. */
        create_surface(a, o);
    }
#undef BIND
#undef IS
}

static void registry_global_remove(void *data, [[maybe_unused]] struct wl_registry *reg,
                                   uint32_t name) {
    app *a = data;
    XD_LIST_FOR_EACH_SAFE(o, tmp, &a->outputs, output, link) {
        if (o->global == name)
            destroy_output(o);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---- main --------------------------------------------------------------------- */

static void who_am_i(app *a) {
    struct passwd *pw = getpwuid(getuid());
    snprintf(a->user, sizeof(a->user), "%s", pw != nullptr ? pw->pw_name : "user");
    char host[256] = "localhost";
    gethostname(host, sizeof(host) - 1);
    char full[256] = "";
    if (pw != nullptr && pw->pw_gecos != nullptr) {
        snprintf(full, sizeof(full), "%s", pw->pw_gecos);
        char *comma = strchr(full, ',');
        if (comma != nullptr)
            *comma = '\0';
    }
    if (full[0] != '\0' && strcmp(full, a->user) != 0)
        snprintf(a->who, sizeof(a->who), "%s@%s (%s)", a->user, host, full);
    else
        snprintf(a->who, sizeof(a->who), "%s@%s", a->user, host);
}

static bool flush(struct wl_display *d) {
    while (wl_display_flush(d) < 0) {
        if (errno != EAGAIN)
            return false;
        struct pollfd pfd = {.fd = wl_display_get_fd(d), .events = POLLOUT};
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR)
            return false;
    }
    return true;
}

static void run(app *a) {
    struct wl_display *d = a->display;
    while (a->running) {
        XD_LIST_FOR_EACH(o, &a->outputs, output, link) {
            if (o->configured && o->dirty)
                render(o);
        }
        while (wl_display_prepare_read(d) != 0)
            wl_display_dispatch_pending(d);
        if (!flush(d)) {
            wl_display_cancel_read(d);
            break;
        }
        struct pollfd fds[4] = {
            {.fd = wl_display_get_fd(d), .events = POLLIN},
            {.fd = a->pam_pipe[0], .events = POLLIN},
            {.fd = a->idle_fd, .events = POLLIN},
            {.fd = a->frame_fd, .events = POLLIN},
        };
        if (poll(fds, 4, -1) < 0) {
            wl_display_cancel_read(d);
            if (errno == EINTR)
                continue;
            break;
        }
        bool ok = true;
        if (fds[0].revents & POLLIN)
            ok = wl_display_read_events(d) >= 0;
        else
            wl_display_cancel_read(d);
        if (!ok || (fds[0].revents & (POLLERR | POLLHUP)) || wl_display_dispatch_pending(d) < 0) {
            fprintf(stderr, "xd-lock: lost the compositor\n");
            a->exit_code = EXIT_FAILURE;
            break;
        }
        if (fds[1].revents & POLLIN)
            check_result(a);
        if (fds[2].revents & POLLIN) {
            uint64_t n;
            (void)!read(a->idle_fd, &n, sizeof(n));
            if (a->screen != SCREEN_CHECKING && a->saver_id[0] != '\0')
                go(a, SCREEN_SAVER);
        }
        if (fds[3].revents & POLLIN) {
            uint64_t n;
            (void)!read(a->frame_fd, &n, sizeof(n));
            XD_LIST_FOR_EACH(o, &a->outputs, output, link) {
                if (o->throttled && a->screen == SCREEN_SAVER)
                    o->dirty = true;
                o->throttled = false;
            }
        }
    }
    flush(d);
}

static void usage(void) {
    fprintf(stderr, "usage: xd-lock [--saver [ID] [--lock|--no-lock]]\n");
}

int main(int argc, char **argv) {
    /* The password passes through this process: keep it out of core
     * dumps and swap. */
    prctl(PR_SET_DUMPABLE, 0);

    static app a;
    wl_list_init(&a.outputs);
    a.exit_code = EXIT_SUCCESS;
    xd_settings_load(&a.settings);
    xd_settings_scheme(&a.settings, &a.scheme);

    bool saver = false;
    int lock_opt = -1; /* -1: from settings */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--saver") == 0) {
            saver = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                snprintf(a.saver_id, sizeof(a.saver_id), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--lock") == 0) {
            lock_opt = 1;
        } else if (strcmp(argv[i], "--no-lock") == 0) {
            lock_opt = 0;
        } else if (strcmp(argv[i], "--style") == 0 && i + 1 < argc) {
            /* The shell passes on its own overrides. */
            if (!xd_visual_style_from_name(xd_str_from_cstr(argv[++i]), &a.settings.style))
                fprintf(stderr, "xd-lock: unknown style %s\n", argv[i]);
        } else if (strcmp(argv[i], "--scheme") == 0 && i + 1 < argc) {
            xd_strbuf err = {0};
            if (!xd_scheme_load(argv[++i], &a.scheme, &err))
                fprintf(stderr, "xd-lock: %s: %s\n", argv[i], xd_strbuf_cstr(&err));
            xd_strbuf_free(&err);
        } else {
            usage();
            return EXIT_FAILURE;
        }
    }
    if (saver && a.saver_id[0] == '\0')
        snprintf(a.saver_id, sizeof(a.saver_id), "%s", a.settings.screensaver);
    if (saver && xd_saver_find(a.saver_id) == nullptr) {
        fprintf(stderr, "xd-lock: no screen saver \"%s\"\n", a.saver_id);
        return EXIT_FAILURE;
    }
    if (saver) {
        a.locking = lock_opt >= 0 ? lock_opt == 1 : a.settings.screensaver_password;
        a.screen = SCREEN_SAVER;
        a.idle_seconds = 30; /* an abandoned unlock dialog gives way to the saver */
    } else {
        a.locking = true;
        a.screen = SCREEN_LOCKED;
        /* A locked computer runs the screen saver after its usual wait. */
        if (xd_saver_find(a.settings.screensaver) != nullptr) {
            snprintf(a.saver_id, sizeof(a.saver_id), "%s", a.settings.screensaver);
            a.idle_seconds = XD_MAX(a.settings.screensaver_wait, 1) * 60;
        }
    }
    who_am_i(&a);

    a.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    a.display = wl_display_connect(nullptr);
    if (a.display == nullptr) {
        fprintf(stderr, "xd-lock: cannot connect to a Wayland compositor\n");
        return EXIT_FAILURE;
    }
    struct wl_registry *reg = wl_display_get_registry(a.display);
    wl_registry_add_listener(reg, &registry_listener, &a);
    wl_display_roundtrip(a.display);
    if (a.compositor == nullptr || a.shm == nullptr) {
        fprintf(stderr, "xd-lock: compositor lacks wl_shm\n");
        return EXIT_FAILURE;
    }
    if (a.locking && a.lock_manager == nullptr) {
        fprintf(stderr, "xd-lock: the compositor can't lock the screen (ext-session-lock)\n");
        return EXIT_FAILURE;
    }
    if (!a.locking && a.layer_shell == nullptr) {
        fprintf(stderr, "xd-lock: the compositor lacks layer-shell\n");
        return EXIT_FAILURE;
    }
    if (pipe2(a.pam_pipe, O_CLOEXEC) < 0) {
        perror("xd-lock: pipe");
        return EXIT_FAILURE;
    }
    a.idle_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    a.frame_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    a.icons = xd_icons_create(xd_settings_icon_theme(&a.settings));

    if (a.locking) {
        a.lock = ext_session_lock_manager_v1_lock(a.lock_manager);
        ext_session_lock_v1_add_listener(a.lock, &lock_listener, &a);
    }
    XD_LIST_FOR_EACH(o, &a.outputs, output, link) {
        create_surface(&a, o);
    }
    a.running = true;
    arm_idle(&a);
    run(&a);

    if (a.pam_running)
        pthread_join(a.pam_thread, nullptr);
    explicit_bzero(a.password, sizeof(a.password));
    XD_LIST_FOR_EACH_SAFE(o, tmp, &a.outputs, output, link) {
        destroy_output(o);
    }
    /* Still locked (lost the compositor, or it ended the lock): just
     * disconnect. Destroying a held lock would be a protocol error, and
     * the compositor keeps the screen locked when we go. */
    if (a.lock != nullptr && !a.locked)
        ext_session_lock_v1_destroy(a.lock);
    xd_icons_destroy(a.icons);
    xkb_state_unref(a.xkb_state);
    xkb_keymap_unref(a.keymap);
    xkb_context_unref(a.xkb);
    close(a.idle_fd);
    close(a.frame_fd);
    close(a.pam_pipe[0]);
    close(a.pam_pipe[1]);
    wl_display_disconnect(a.display);
    return a.exit_code;
}
