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
 *
 * logind speaks for the display manager: after Switch User its greeter
 * checks the password, then asks logind to unlock our session, which
 * sends the session's Unlock signal. We unlock then, so returning doesn't
 * ask for the password twice; Lock (loginctl lock-session, or the display
 * manager) turns a running unprotected saver into a lock. LockedHint tells
 * logind's other clients whether we're holding the lock. A thread of its
 * own runs the D-Bus side, so the loop below stays plain poll().
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdatomic.h>
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
#include "../settings/login1.h"
#include "../settings/settings.h"
#include "../settings/themepkg.h"
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

/* What the logind thread tells the main loop, a byte each. */
enum : char { LOGIND_LOCK = 'L', LOGIND_UNLOCK = 'U' };

/* The logind thread. The main thread shares only `pipe`, `cancel`,
 * `locking` and `unlocked` with it. */
typedef struct logind_watch {
    bool started;
    pthread_t thread;
    int pipe[2];
    GCancellable *cancel;
    atomic_bool locking;  /* we hold, or are taking, the session lock */
    atomic_bool unlocked; /* leaving unlocked (not crashed or lost) */
    /* the thread's own */
    GDBusConnection *bus;
    char *session; /* our session's object path */
    bool hinted;   /* LockedHint set */
} logind_watch;

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

    logind_watch logind;
};

/* ---- PAM -------------------------------------------------------------------- */

static int pam_conv_cb(int count, const struct pam_message **msg, struct pam_response **resp,
                       void *data) {
    const char *password = data;
    if (count <= 0 || count > PAM_MAX_NUM_MSG)
        return PAM_CONV_ERR;
    struct pam_response *responses = calloc((size_t)count, sizeof(*responses));
    if (responses == nullptr)
        return PAM_BUF_ERR;
    for (int i = 0; i < count; i++) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            responses[i].resp = strdup(password);
            if (responses[i].resp == nullptr) {
                for (int j = 0; j < i; j++)
                    free(responses[j].resp);
                free(responses);
                return PAM_BUF_ERR;
            }
        }
    }
    *resp = responses;
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
    app *locker = data;
    struct pam_conv conv = {.conv = pam_conv_cb, .appdata_ptr = locker->pam_password};
    pam_handle_t *pam_handle = nullptr;
    int pam_status = pam_start(pam_service(), locker->user, &conv, &pam_handle);
    if (pam_status == PAM_SUCCESS)
        pam_status = pam_authenticate(pam_handle, 0);
    if (pam_handle != nullptr)
        pam_end(pam_handle, pam_status);
    explicit_bzero(locker->pam_password, sizeof(locker->pam_password));
    char ok = pam_status == PAM_SUCCESS ? 1 : 0;
    (void)!write(locker->pam_pipe[1], &ok, 1);
    return nullptr;
}

static void start_check(app *locker) {
    if (locker->pam_running)
        return;
    memcpy(locker->pam_password, locker->password, sizeof(locker->password));
    explicit_bzero(locker->password, sizeof(locker->password));
    locker->password_len = 0;
    locker->pam_running = true;
    locker->screen = SCREEN_CHECKING;
    if (pthread_create(&locker->pam_thread, nullptr, pam_worker, locker) != 0) {
        locker->pam_running = false;
        explicit_bzero(locker->pam_password, sizeof(locker->pam_password));
        locker->screen = SCREEN_FAILED;
    }
}

/* ---- screens ------------------------------------------------------------------ */

static void redraw_all(app *locker) {
    XD_LIST_FOR_EACH(screen_output, &locker->outputs, output, link) { screen_output->dirty = true; }
}

static void set_cursor(app *locker) {
    if (locker->pointer == nullptr)
        return;
    if (locker->screen == SCREEN_SAVER)
        wl_pointer_set_cursor(locker->pointer, locker->pointer_serial, nullptr, 0, 0);
    else if (locker->cursor_shape != nullptr)
        wp_cursor_shape_device_v1_set_shape(locker->cursor_shape, locker->pointer_serial,
                                            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

static void arm_idle(app *locker) {
    struct itimerspec spec = {0};
    if (locker->idle_seconds > 0 && locker->screen != SCREEN_SAVER && locker->saver_id[0] != '\0')
        spec.it_value.tv_sec = locker->idle_seconds;
    timerfd_settime(locker->idle_fd, 0, &spec, nullptr);
}

static void go(app *locker, screen next_screen) {
    if (next_screen != SCREEN_UNLOCK && next_screen != SCREEN_FAILED &&
        next_screen != SCREEN_CHECKING) {
        explicit_bzero(locker->password, sizeof(locker->password));
        locker->password_len = 0;
    }
    locker->screen = next_screen;
    /* Pointer movement ends a saver only beyond a small distance from
     * where the pointer is now. */
    locker->pointer_seen = locker->pointer_output != nullptr;
    locker->pointer_x0 = locker->pointer_x;
    locker->pointer_y0 = locker->pointer_y;
    locker->pressed = HIT_NONE;
    set_cursor(locker);
    arm_idle(locker);
    redraw_all(locker);
}

static void unlock(app *locker) {
    if (locker->lock != nullptr) {
        if (locker->locked)
            ext_session_lock_v1_unlock_and_destroy(locker->lock);
        else
            ext_session_lock_v1_destroy(locker->lock);
        locker->lock = nullptr;
        wl_display_roundtrip(locker->display);
    }
    locker->running = false;
}

/* Any input while the saver runs. */
static void saver_input(app *locker) {
    if (!locker->locking)
        locker->running = false;
    else
        go(locker, SCREEN_LOCKED);
}

static void check_result(app *locker) {
    char ok = 0;
    (void)!read(locker->pam_pipe[0], &ok, 1);
    if (locker->pam_running) {
        pthread_join(locker->pam_thread, nullptr);
        locker->pam_running = false;
    }
    if (ok)
        unlock(locker);
    else
        go(locker, SCREEN_FAILED);
}

/* ---- drawing ------------------------------------------------------------------ */

static double output_scale(const output *screen_output) {
    return screen_output->frac_scale > 0 ? screen_output->frac_scale
                                         : (double)XD_MAX(screen_output->int_scale, 1);
}

static void draw_icon(app *locker, cairo_t *cairo, const char *name, int x, int y, int px) {
    static const char *const fallback[] = {"system-lock-screen", "dialog-password",
                                           "dialog-information"};
    cairo_surface_t *icon = xd_icons_get(locker->icons, name, px);
    if (icon == nullptr)
        icon = xd_icons_get_any(locker->icons, fallback, XD_ARRAY_LEN(fallback), px);
    if (icon != nullptr) {
        cairo_set_source_surface(cairo, icon, x, y);
        cairo_paint(cairo);
    }
}

/* Logical -> device rect relative to an origin. */
static xd_rect L(const xd_theme *theme, xd_rect origin, int x, int y, int width, int height) {
    return (xd_rect){origin.x + xd_tpx(theme, x), origin.y + xd_tpx(theme, y), xd_tpx(theme, width),
                     xd_tpx(theme, height)};
}

/* A centred dialog with a frame; returns its client area. */
static xd_rect dialog(const xd_theme *theme, cairo_t *cairo, int dev_w, int dev_h,
                      const char *title, int client_w, int client_h) {
    xd_frame frame = {.title = title, .active = true, .dialog = true};
    xd_insets in = xd_theme_frame_insets(theme, &frame);
    int width = xd_tpx(theme, client_w) + in.left + in.right;
    int height = xd_tpx(theme, client_h) + in.top + in.bottom;
    xd_rect outer = {(dev_w - width) / 2, (dev_h - height) / 2, width, height};
    xd_theme_draw_frame(theme, cairo, outer, &frame);
    xd_rect client = xd_rect_inset4(outer, in.left, in.top, in.right, in.bottom);
    xd_fill_rect(cairo, client, xd_theme_dialog_color(theme));
    return client;
}

static void draw_text(const xd_theme *theme, cairo_t *cairo, xd_rect rect, const char *text,
                      bool wrap, bool dim) {
    xd_text_style st =
        xd_text(theme, XD_FONT_MESSAGE,
                xd_sys(theme, dim ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT), XD_ALIGN_START);
    st.wrap = wrap;
    st.ellipsize = !wrap;
    xd_draw_text(cairo, rect, text, &st);
}

static void draw_field(const xd_theme *theme, cairo_t *cairo, xd_rect rect, const char *text,
                       bool disabled, bool caret) {
    xd_rect in = xd_theme_draw_field(theme, cairo, rect, disabled);
    xd_text_style st = xd_text(theme, XD_FONT_MESSAGE,
                               xd_sys(theme, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_WINDOW_TEXT),
                               XD_ALIGN_START);
    xd_rect tr = xd_rect_inset4(in, xd_tpx(theme, 2), 0, xd_tpx(theme, 2), 0);
    xd_draw_text(cairo, tr, text, &st);
    if (caret) {
        int x = tr.x + xd_text_width(cairo, text, &st) + xd_tpx(theme, 1);
        xd_fill_rect(
            cairo, (xd_rect){x, in.y + xd_tpx(theme, 2), xd_tpx(theme, 1), in.h - xd_tpx(theme, 4)},
            xd_sys(theme, XD_COLOR_WINDOW_TEXT));
    }
}

static bool caps_lock(const app *locker) {
    return locker->xkb_state != nullptr &&
           xkb_state_mod_name_is_active(locker->xkb_state, XKB_MOD_NAME_CAPS,
                                        XKB_STATE_MODS_LOCKED) > 0;
}

static void draw_lock_dialog(app *locker, const xd_theme *theme, cairo_t *cairo, int dw, int dh) {
    bool unlock_form = locker->screen != SCREEN_LOCKED;
    enum { W = 412 };
    int client_h = unlock_form ? 268 : 196;
    xd_rect dialog_rect = dialog(theme, cairo, dw, dh,
                                 unlock_form ? "Unlock Computer" : "Computer Locked", W, client_h);
    xd_theme_draw_dialog_banner(theme, cairo, L(theme, dialog_rect, 0, 0, W, 72), "XeonDesktop",
                                "Professional");
    draw_icon(locker, cairo, "system-lock-screen", dialog_rect.x + xd_tpx(theme, 14),
              dialog_rect.y + xd_tpx(theme, 86), xd_tpx(theme, 32));
    draw_text(theme, cairo, L(theme, dialog_rect, 60, 86, W - 74, 16),
              "This computer is in use and has been locked.", false, false);
    char only[900];
    snprintf(only, sizeof(only), "Only %s or an administrator can unlock this computer.",
             locker->who);
    draw_text(theme, cairo, L(theme, dialog_rect, 60, 110, W - 74, 32), only, true, false);

    if (!unlock_form) {
        draw_text(theme, cairo, L(theme, dialog_rect, 60, 154, W - 74, 16),
                  "Press Ctrl-Alt-Del to unlock this computer.", false, false);
        locker->ok_rect = locker->cancel_rect = (xd_rect){0};
        return;
    }
    bool busy = locker->screen == SCREEN_CHECKING;
    draw_text(theme, cairo, L(theme, dialog_rect, 60, 157, 76, 16), "User name:", false, busy);
    draw_field(theme, cairo, L(theme, dialog_rect, 140, 154, W - 154, 21), locker->user, true,
               false);
    draw_text(theme, cairo, L(theme, dialog_rect, 60, 185, 76, 16), "Password:", false, busy);
    char stars[sizeof(locker->password) * 3 + 1];
    size_t count = 0;
    const char *bullet = theme->style == XD_STYLE_CLASSIC ? "*" : "●";
    for (size_t i = 0; i < locker->password_len && count + 4 < sizeof(stars); i++) {
        size_t bl = strlen(bullet);
        memcpy(stars + count, bullet, bl);
        count += bl;
    }
    stars[count] = '\0';
    draw_field(theme, cairo, L(theme, dialog_rect, 140, 182, W - 154, 21), stars, busy,
               locker->screen == SCREEN_UNLOCK);
    if (busy)
        draw_text(theme, cairo, L(theme, dialog_rect, 140, 207, W - 154, 16),
                  "Checking your password...", false, true);
    else if (caps_lock(locker))
        draw_text(theme, cairo, L(theme, dialog_rect, 140, 207, W - 154, 16), "Caps Lock is on.",
                  false, true);

    locker->ok_rect = L(theme, dialog_rect, W - 170, 234, 75, 23);
    locker->cancel_rect = L(theme, dialog_rect, W - 89, 234, 75, 23);
    uint8_t dis = busy || locker->screen == SCREEN_FAILED ? XD_STATE_DISABLED : 0;
    xd_theme_draw_button(
        theme, cairo, locker->ok_rect,
        (uint8_t)(dis | XD_STATE_DEFAULT | (locker->pressed == HIT_OK ? XD_STATE_PRESSED : 0)),
        "OK");
    xd_theme_draw_button(theme, cairo, locker->cancel_rect,
                         (uint8_t)(dis | (locker->pressed == HIT_CANCEL ? XD_STATE_PRESSED : 0)),
                         "Cancel");

    if (locker->screen == SCREEN_FAILED) {
        enum { MW = 340 };
        xd_rect message_rect = dialog(theme, cairo, dw, dh, "Unlock Computer", MW, 112);
        draw_icon(locker, cairo, "dialog-error", message_rect.x + xd_tpx(theme, 12),
                  message_rect.y + xd_tpx(theme, 14), xd_tpx(theme, 32));
        draw_text(theme, cairo, L(theme, message_rect, 56, 14, MW - 68, 56),
                  "The system could not log you on. Make sure your password is correct, then "
                  "type it again. Letters in passwords must be typed using the correct case.",
                  true, false);
        locker->ok_rect = L(theme, message_rect, (MW - 75) / 2, 78, 75, 23);
        locker->cancel_rect = (xd_rect){0};
        xd_theme_draw_button(
            theme, cairo, locker->ok_rect,
            (uint8_t)(XD_STATE_DEFAULT | (locker->pressed == HIT_OK ? XD_STATE_PRESSED : 0)), "OK");
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
    output *screen_output = data;
    app *locker = screen_output->app;
    wl_callback_destroy(cb);
    screen_output->frame = nullptr;
    if (locker->screen != SCREEN_SAVER)
        return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double wait = 1.0 / SAVER_FPS - elapsed(&screen_output->last_frame, &now);
    if (wait <= 0.002) {
        screen_output->dirty = true;
        return;
    }
    screen_output->throttled = true;
    struct itimerspec cur;
    timerfd_gettime(locker->frame_fd, &cur);
    long ns = (long)(wait * 1e9);
    if ((cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0) ||
        (cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > ns)) {
        struct itimerspec spec = {.it_value = {.tv_nsec = ns}};
        timerfd_settime(locker->frame_fd, 0, &spec, nullptr);
    }
}

static void render(output *screen_output) {
    app *locker = screen_output->app;
    double scale = output_scale(screen_output);
    int dw = (int)lround(screen_output->width * scale);
    int dh = (int)lround(screen_output->height * scale);
    if (dw <= 0 || dh <= 0)
        return;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&screen_output->ring, locker->shm, dw, dh);
    if (buf == nullptr)
        return; /* drawn when a buffer comes back */
    cairo_t *cairo = cairo_create(buf->surface);
    xd_theme theme;
    xd_theme_init(&theme, locker->settings.style, &locker->scheme, scale);

    if (locker->screen == SCREEN_SAVER) {
        if (screen_output->saver == nullptr || screen_output->saver_w != dw ||
            screen_output->saver_h != dh) {
            if (screen_output->saver != nullptr)
                xd_saver_destroy(screen_output->saver);
            screen_output->saver =
                xd_saver_create(locker->saver_id, &locker->settings.savers, dw, dh, scale);
            screen_output->saver_w = dw;
            screen_output->saver_h = dh;
            screen_output->last_frame = (struct timespec){0};
        }
        double dt = seconds_since(&screen_output->last_frame);
        if (screen_output->saver != nullptr)
            xd_saver_frame(screen_output->saver, cairo, dt);
        else
            xd_fill_rect(cairo, (xd_rect){0, 0, dw, dh}, xd_rgba_hex(0x000000));
    } else {
        /* The logon desktop: the scheme's background colour, no wallpaper. */
        xd_fill_rect(cairo, (xd_rect){0, 0, dw, dh}, xd_sys(&theme, XD_COLOR_BACKGROUND));
        if (screen_output == locker->dialog_output)
            draw_lock_dialog(locker, &theme, cairo, dw, dh);
    }
    cairo_destroy(cairo);
    cairo_surface_flush(buf->surface);

    /* A still saver (Blank Screen) is drawn once, not every frame. */
    if (locker->screen == SCREEN_SAVER && screen_output->frame == nullptr &&
        xd_saver_animated(locker->saver_id)) {
        screen_output->frame = wl_surface_frame(screen_output->surface);
        wl_callback_add_listener(screen_output->frame, &frame_listener, screen_output);
    }
    wl_surface_attach(screen_output->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(screen_output->surface, 0, 0, dw, dh);
    if (screen_output->viewport != nullptr)
        wp_viewport_set_destination(screen_output->viewport, screen_output->width,
                                    screen_output->height);
    else
        wl_surface_set_buffer_scale(screen_output->surface, XD_MAX(screen_output->int_scale, 1));
    wl_surface_commit(screen_output->surface);
    buf->busy = true;
    screen_output->dirty = false;
}

/* ---- surfaces ----------------------------------------------------------------- */

static void configured(output *screen_output, int width, int height) {
    screen_output->width = width;
    screen_output->height = height;
    screen_output->configured = true;
    screen_output->dirty = true;
    app *locker = screen_output->app;
    if (locker->dialog_output == nullptr)
        locker->dialog_output = screen_output;
}

static void lock_surface_configure(void *data, struct ext_session_lock_surface_v1 *ls,
                                   uint32_t serial, uint32_t width, uint32_t height) {
    ext_session_lock_surface_v1_ack_configure(ls, serial);
    configured(data, (int)width, (int)height);
}

static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
    .configure = lock_surface_configure,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
                            uint32_t width, uint32_t height) {
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    configured(data, (int)width, (int)height);
}

static void layer_closed(void *data, [[maybe_unused]] struct zwlr_layer_surface_v1 *ls) {
    output *screen_output = data;
    screen_output->app->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void fractional_scale(void *data, [[maybe_unused]] struct wp_fractional_scale_v1 *fractional,
                             uint32_t scale) {
    output *screen_output = data;
    screen_output->frac_scale = scale / 120.0;
    screen_output->dirty = true;
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = fractional_scale,
};

static void create_surface(app *locker, output *screen_output) {
    if (screen_output->surface != nullptr || locker->compositor == nullptr)
        return;
    if (locker->locking && locker->lock == nullptr)
        return;
    screen_output->surface = wl_compositor_create_surface(locker->compositor);
    if (locker->viewporter != nullptr && locker->fractional_manager != nullptr) {
        screen_output->viewport =
            wp_viewporter_get_viewport(locker->viewporter, screen_output->surface);
        screen_output->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
            locker->fractional_manager, screen_output->surface);
        wp_fractional_scale_v1_add_listener(screen_output->fractional, &fractional_listener,
                                            screen_output);
    }
    if (locker->locking) {
        screen_output->lock_surface = ext_session_lock_v1_get_lock_surface(
            locker->lock, screen_output->surface, screen_output->wl_output);
        ext_session_lock_surface_v1_add_listener(screen_output->lock_surface,
                                                 &lock_surface_listener, screen_output);
    } else {
        screen_output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            locker->layer_shell, screen_output->surface, screen_output->wl_output,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "screensaver");
        zwlr_layer_surface_v1_set_anchor(
            screen_output->layer_surface,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(screen_output->layer_surface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            screen_output->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
        zwlr_layer_surface_v1_add_listener(screen_output->layer_surface, &layer_listener,
                                           screen_output);
        wl_surface_commit(screen_output->surface);
    }
}

/* The output's surface and everything on it; the output stays. */
static void drop_surface(output *screen_output) {
    app *locker = screen_output->app;
    if (locker->pointer_output == screen_output)
        locker->pointer_output = nullptr;
    if (screen_output->frame != nullptr)
        wl_callback_destroy(screen_output->frame);
    if (screen_output->saver != nullptr)
        xd_saver_destroy(screen_output->saver);
    if (screen_output->lock_surface != nullptr)
        ext_session_lock_surface_v1_destroy(screen_output->lock_surface);
    if (screen_output->layer_surface != nullptr)
        zwlr_layer_surface_v1_destroy(screen_output->layer_surface);
    if (screen_output->fractional != nullptr)
        wp_fractional_scale_v1_destroy(screen_output->fractional);
    if (screen_output->viewport != nullptr)
        wp_viewport_destroy(screen_output->viewport);
    if (screen_output->surface != nullptr)
        wl_surface_destroy(screen_output->surface);
    xd_shm_ring_finish(&screen_output->ring);
    screen_output->frame = nullptr;
    screen_output->saver = nullptr;
    screen_output->lock_surface = nullptr;
    screen_output->layer_surface = nullptr;
    screen_output->fractional = nullptr;
    screen_output->viewport = nullptr;
    screen_output->surface = nullptr;
    screen_output->configured = false;
    screen_output->throttled = false;
}

static void destroy_output(output *screen_output) {
    app *locker = screen_output->app;
    if (locker->dialog_output == screen_output) {
        locker->dialog_output = nullptr;
        XD_LIST_FOR_EACH(other, &locker->outputs, output, link) {
            if (other != screen_output && other->configured && locker->dialog_output == nullptr) {
                locker->dialog_output = other;
                other->dirty = true;
            }
        }
    }
    drop_surface(screen_output);
    wl_output_release(screen_output->wl_output);
    wl_list_remove(&screen_output->link);
    xd_free(screen_output);
}

/* ---- session lock ---------------------------------------------------------------- */

static void lock_locked(void *data, [[maybe_unused]] struct ext_session_lock_v1 *session_lock) {
    app *locker = data;
    locker->locked = true;
}

static void lock_finished(void *data, [[maybe_unused]] struct ext_session_lock_v1 *session_lock) {
    app *locker = data;
    if (!locker->locked)
        fprintf(stderr, "xd-lock: the compositor refused to lock (another locker running?)\n");
    else
        fprintf(stderr, "xd-lock: the compositor ended the lock\n");
    locker->exit_code = locker->locked ? EXIT_SUCCESS : EXIT_FAILURE;
    locker->running = false;
}

static const struct ext_session_lock_v1_listener lock_listener = {
    .locked = lock_locked,
    .finished = lock_finished,
};

/* An unprotected saver becomes a lock: the overlay surfaces give way to
 * lock surfaces, and the saver keeps running on them. */
static void begin_lock(app *locker) {
    if (locker->locking)
        return;
    if (locker->lock_manager == nullptr) {
        fprintf(stderr, "xd-lock: the compositor can't lock the screen (ext-session-lock)\n");
        return;
    }
    XD_LIST_FOR_EACH(screen_output, &locker->outputs, output, link) { drop_surface(screen_output); }
    locker->dialog_output = nullptr;
    locker->locking = true;
    atomic_store(&locker->logind.locking, true);
    locker->lock = ext_session_lock_manager_v1_lock(locker->lock_manager);
    ext_session_lock_v1_add_listener(locker->lock, &lock_listener, locker);
    XD_LIST_FOR_EACH(screen_output, &locker->outputs, output, link) {
        create_surface(locker, screen_output);
    }
}

/* ---- logind ------------------------------------------------------------------- */

static void set_locked_hint(logind_watch *watch, bool locked, GCancellable *cancel) {
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_sync(
        watch->bus, XD_LOGIN1, watch->session, XD_LOGIN1_SESSION, "SetLockedHint",
        g_variant_new("(b)", locked), nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, cancel, &error);
    if (variant != nullptr) {
        g_variant_unref(variant);
        watch->hinted = locked;
    } else {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            fprintf(stderr, "xd-lock: cannot set logind's LockedHint: %s\n", error->message);
        g_error_free(error);
    }
}

static void logind_signal([[maybe_unused]] GDBusConnection *bus,
                          [[maybe_unused]] const char *sender, [[maybe_unused]] const char *path,
                          [[maybe_unused]] const char *iface, const char *member,
                          [[maybe_unused]] GVariant *params, gpointer data) {
    logind_watch *watch = data;
    char message = 0;
    if (strcmp(member, "Unlock") == 0) {
        message = LOGIND_UNLOCK;
    } else if (strcmp(member, "Lock") == 0) {
        message = LOGIND_LOCK;
        if (!watch->hinted)
            set_locked_hint(watch, true, watch->cancel);
    }
    if (message != 0)
        (void)!write(watch->pipe[1], &message, 1);
}

static gboolean logind_cancelled([[maybe_unused]] GCancellable *cancellable, gpointer loop) {
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void *logind_thread(void *data) {
    logind_watch *watch = data;
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GMainLoop *loop = g_main_loop_new(ctx, FALSE);
    /* Quits the loop when the main thread cancels, even before it runs. */
    GSource *quit = g_cancellable_source_new(watch->cancel);
    g_source_set_callback(quit, G_SOURCE_FUNC(logind_cancelled), loop, nullptr);
    g_source_attach(quit, ctx);

    GError *error = nullptr;
    watch->bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, watch->cancel, &error);
    if (watch->bus != nullptr)
        watch->session = xd_login1_own_session(watch->bus, watch->cancel, &error);
    if (watch->session != nullptr) {
        /* Signals come to this thread's context. */
        guint sub = g_dbus_connection_signal_subscribe(
            watch->bus, XD_LOGIN1, XD_LOGIN1_SESSION, nullptr, watch->session, nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, logind_signal, watch, nullptr);
        if (atomic_load(&watch->locking))
            set_locked_hint(watch, true, watch->cancel);
        g_main_loop_run(loop);
        g_dbus_connection_signal_unsubscribe(watch->bus, sub);
        /* Cleared only if we really let go: a lost lock stays locked for
         * the compositor's next locker. */
        if (watch->hinted && atomic_load(&watch->unlocked))
            set_locked_hint(watch, false, nullptr);
    } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        fprintf(stderr, "xd-lock: not following logind's Lock and Unlock: %s\n",
                error != nullptr ? error->message : "no session");
    }
    g_clear_error(&error);
    g_clear_pointer(&watch->session, g_free);
    g_clear_object(&watch->bus);
    g_source_destroy(quit);
    g_source_unref(quit);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    return nullptr;
}

/* Only in a real session (or a test's mock logind), as for the shell's
 * sleep and Switch User: a nested session's logind is the host's. */
static void logind_start(app *locker) {
    logind_watch *watch = &locker->logind;
    watch->pipe[0] = watch->pipe[1] = -1;
    atomic_store(&watch->locking, locker->locking);
    if (xd_login1_simulated())
        return;
    if (pipe2(watch->pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
        watch->pipe[0] = watch->pipe[1] = -1;
        return;
    }
    watch->cancel = g_cancellable_new();
    watch->started = pthread_create(&watch->thread, nullptr, logind_thread, watch) == 0;
}

static void logind_stop(app *locker) {
    logind_watch *watch = &locker->logind;
    if (watch->started) {
        g_cancellable_cancel(watch->cancel);
        pthread_join(watch->thread, nullptr);
    }
    g_clear_object(&watch->cancel);
    if (watch->pipe[0] >= 0) {
        close(watch->pipe[0]);
        close(watch->pipe[1]);
    }
}

static void logind_message(app *locker) {
    char message = 0;
    while (read(locker->logind.pipe[0], &message, 1) == 1) {
        if (message == LOGIND_UNLOCK) {
            /* The display manager's greeter checked the password. A PAM
             * check of our own may still be running; main joins it. */
            fprintf(stderr, "xd-lock: logind unlocked the session\n");
            unlock(locker);
        } else if (message == LOGIND_LOCK && !locker->locking) {
            begin_lock(locker);
        }
    }
}

/* ---- input -------------------------------------------------------------------- */

static void user_activity(app *locker) { arm_idle(locker); }

static hit hit_at(const app *locker, const output *screen_output, double x, double y) {
    if (screen_output != locker->dialog_output)
        return HIT_NONE;
    double scale = output_scale(screen_output);
    double dx = x * scale;
    double dy = y * scale;
    const xd_rect *rs[] = {&locker->ok_rect, &locker->cancel_rect};
    for (size_t i = 0; i < 2; i++) {
        const xd_rect *rect = rs[i];
        if (rect->w > 0 && dx >= rect->x && dy >= rect->y && dx < rect->x + rect->w &&
            dy < rect->y + rect->h)
            return i == 0 ? HIT_OK : HIT_CANCEL;
    }
    return HIT_NONE;
}

static void press_ok(app *locker) {
    if (locker->screen == SCREEN_FAILED)
        go(locker, SCREEN_UNLOCK);
    else if (locker->screen == SCREEN_UNLOCK) {
        start_check(locker);
        redraw_all(locker);
    }
}

static void pointer_enter(void *data, [[maybe_unused]] struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    app *locker = data;
    locker->pointer_serial = serial;
    locker->pointer_output = nullptr;
    XD_LIST_FOR_EACH(screen_output, &locker->outputs, output, link) {
        if (screen_output->surface == surface)
            locker->pointer_output = screen_output;
    }
    locker->pointer_x = wl_fixed_to_double(x);
    locker->pointer_y = wl_fixed_to_double(y);
    if (!locker->pointer_seen) {
        locker->pointer_seen = true;
        locker->pointer_x0 = locker->pointer_x;
        locker->pointer_y0 = locker->pointer_y;
    }
    set_cursor(locker);
}

static void pointer_leave(void *data, [[maybe_unused]] struct wl_pointer *pointer,
                          [[maybe_unused]] uint32_t serial,
                          [[maybe_unused]] struct wl_surface *surface) {
    app *locker = data;
    locker->pointer_output = nullptr;
}

static void pointer_motion(void *data, [[maybe_unused]] struct wl_pointer *pointer,
                           [[maybe_unused]] uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    app *locker = data;
    locker->pointer_x = wl_fixed_to_double(x);
    locker->pointer_y = wl_fixed_to_double(y);
    if (locker->screen != SCREEN_SAVER) {
        user_activity(locker);
        return;
    }
    /* Like Windows, ignore a nudge: only a real move ends the saver. */
    if (!locker->pointer_seen) {
        locker->pointer_seen = true;
        locker->pointer_x0 = locker->pointer_x;
        locker->pointer_y0 = locker->pointer_y;
    } else if (fabs(locker->pointer_x - locker->pointer_x0) +
                   fabs(locker->pointer_y - locker->pointer_y0) >
               12) {
        saver_input(locker);
    }
}

static void pointer_button(void *data, [[maybe_unused]] struct wl_pointer *pointer, uint32_t serial,
                           [[maybe_unused]] uint32_t time, [[maybe_unused]] uint32_t button,
                           uint32_t state) {
    app *locker = data;
    locker->pointer_serial = serial;
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (locker->screen == SCREEN_SAVER) {
        if (pressed)
            saver_input(locker);
        return;
    }
    user_activity(locker);
    hit target = locker->pointer_output != nullptr
                     ? hit_at(locker, locker->pointer_output, locker->pointer_x, locker->pointer_y)
                     : HIT_NONE;
    bool usable = locker->screen == SCREEN_UNLOCK || locker->screen == SCREEN_FAILED;
    if (pressed) {
        locker->pressed = usable ? target : HIT_NONE;
        redraw_all(locker);
        return;
    }
    hit was = locker->pressed;
    locker->pressed = HIT_NONE;
    redraw_all(locker);
    if (was == HIT_NONE || was != target)
        return;
    if (was == HIT_OK)
        press_ok(locker);
    else if (was == HIT_CANCEL && locker->screen == SCREEN_UNLOCK)
        go(locker, SCREEN_LOCKED);
}

static void pointer_axis(void *data, [[maybe_unused]] struct wl_pointer *pointer,
                         [[maybe_unused]] uint32_t time, [[maybe_unused]] uint32_t axis,
                         [[maybe_unused]] wl_fixed_t value) {
    app *locker = data;
    if (locker->screen == SCREEN_SAVER)
        saver_input(locker);
}

static void pointer_frame([[maybe_unused]] void *data,
                          [[maybe_unused]] struct wl_pointer *pointer) {}
static void pointer_axis_source([[maybe_unused]] void *data,
                                [[maybe_unused]] struct wl_pointer *pointer,
                                [[maybe_unused]] uint32_t source) {}
static void pointer_axis_stop([[maybe_unused]] void *data,
                              [[maybe_unused]] struct wl_pointer *pointer,
                              [[maybe_unused]] uint32_t time, [[maybe_unused]] uint32_t axis) {}
static void pointer_axis_discrete([[maybe_unused]] void *data,
                                  [[maybe_unused]] struct wl_pointer *pointer,
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

static void keyboard_keymap(void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size) {
    app *locker = data;
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
        keymap = xkb_keymap_new_from_string(locker->xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (keymap == nullptr)
        return;
    xkb_state_unref(locker->xkb_state);
    xkb_keymap_unref(locker->keymap);
    locker->keymap = keymap;
    locker->xkb_state = xkb_state_new(keymap);
}

/* The compositor gives the keyboard to the surface on the monitor with
 * the pointer: the dialog goes there, as Windows puts it where the user
 * is looking rather than always on the first monitor. */
static void keyboard_enter(void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
                           [[maybe_unused]] uint32_t serial, struct wl_surface *surface,
                           [[maybe_unused]] struct wl_array *keys) {
    app *locker = data;
    XD_LIST_FOR_EACH(screen_output, &locker->outputs, output, link) {
        if (screen_output->surface == surface && screen_output->configured &&
            locker->dialog_output != screen_output) {
            if (locker->dialog_output != nullptr)
                locker->dialog_output->dirty = true;
            locker->dialog_output = screen_output;
            screen_output->dirty = true;
            locker->pressed = HIT_NONE;
        }
    }
}
static void keyboard_leave([[maybe_unused]] void *data,
                           [[maybe_unused]] struct wl_keyboard *keyboard,
                           [[maybe_unused]] uint32_t serial,
                           [[maybe_unused]] struct wl_surface *surface) {}

static bool mod_active(const app *locker, const char *name) {
    return xkb_state_mod_name_is_active(locker->xkb_state, name, XKB_STATE_MODS_EFFECTIVE) > 0;
}

static void password_key(app *locker, xkb_keysym_t sym, xkb_keycode_t code) {
    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        press_ok(locker);
        return;
    case XKB_KEY_Escape:
        go(locker, SCREEN_LOCKED);
        return;
    case XKB_KEY_BackSpace:
        if (mod_active(locker, XKB_MOD_NAME_CTRL)) {
            explicit_bzero(locker->password, sizeof(locker->password));
            locker->password_len = 0;
        } else if (locker->password_len > 0) {
            /* Back over one UTF-8 character. */
            do {
                locker->password[--locker->password_len] = '\0';
            } while (locker->password_len > 0 &&
                     (locker->password[locker->password_len] & 0xC0) == 0x80);
        }
        redraw_all(locker);
        return;
    default:
        break;
    }
    char utf8[16] = {0};
    int count = xkb_state_key_get_utf8(locker->xkb_state, code, utf8, sizeof(utf8));
    if (count <= 0 || (unsigned char)utf8[0] < 0x20 || utf8[0] == 0x7F)
        return;
    if (locker->password_len + (size_t)count >= sizeof(locker->password))
        return;
    memcpy(locker->password + locker->password_len, utf8, (size_t)count);
    locker->password_len += (size_t)count;
    explicit_bzero(utf8, sizeof(utf8));
    redraw_all(locker);
}

static void keyboard_key(void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
                         [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                         uint32_t key, uint32_t state) {
    app *locker = data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || locker->xkb_state == nullptr)
        return;
    xkb_keycode_t code = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(locker->xkb_state, code);
    bool modifier = sym == XKB_KEY_Control_L || sym == XKB_KEY_Control_R ||
                    sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R || sym == XKB_KEY_Shift_L ||
                    sym == XKB_KEY_Shift_R || sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R;
    switch (locker->screen) {
    case SCREEN_SAVER:
        saver_input(locker);
        return;
    case SCREEN_LOCKED:
        user_activity(locker);
        if (!modifier && (sym == XKB_KEY_Delete || sym == XKB_KEY_KP_Delete) &&
            mod_active(locker, XKB_MOD_NAME_CTRL) && mod_active(locker, XKB_MOD_NAME_ALT))
            go(locker, SCREEN_UNLOCK);
        return;
    case SCREEN_UNLOCK:
        user_activity(locker);
        password_key(locker, sym, code);
        return;
    case SCREEN_FAILED:
        user_activity(locker);
        if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_Escape ||
            sym == XKB_KEY_space)
            go(locker, SCREEN_UNLOCK);
        return;
    case SCREEN_CHECKING:
        return;
    }
}

static void keyboard_modifiers(void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
                               [[maybe_unused]] uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group) {
    app *locker = data;
    if (locker->xkb_state == nullptr)
        return;
    bool caps = caps_lock(locker);
    xkb_state_update_mask(locker->xkb_state, depressed, latched, locked, 0, 0, group);
    if (caps != caps_lock(locker))
        redraw_all(locker); /* the Caps Lock warning */
}

static void keyboard_repeat_info([[maybe_unused]] void *data,
                                 [[maybe_unused]] struct wl_keyboard *keyboard,
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
    app *locker = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && locker->pointer == nullptr) {
        locker->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(locker->pointer, &pointer_listener, locker);
        if (locker->cursor_shape_manager != nullptr)
            locker->cursor_shape = wp_cursor_shape_manager_v1_get_pointer(
                locker->cursor_shape_manager, locker->pointer);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && locker->keyboard == nullptr) {
        locker->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(locker->keyboard, &keyboard_listener, locker);
    }
    /* A keyboard or mouse unplugged (the last one): our objects go dead,
     * so get new ones when one comes back. */
    if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && locker->keyboard != nullptr) {
        wl_keyboard_release(locker->keyboard);
        locker->keyboard = nullptr;
    }
    if (!(caps & WL_SEAT_CAPABILITY_POINTER) && locker->pointer != nullptr) {
        if (locker->cursor_shape != nullptr) {
            wp_cursor_shape_device_v1_destroy(locker->cursor_shape);
            locker->cursor_shape = nullptr;
        }
        wl_pointer_release(locker->pointer);
        locker->pointer = nullptr;
        locker->pointer_output = nullptr;
    }
}

static void seat_name([[maybe_unused]] void *data, [[maybe_unused]] struct wl_seat *seat,
                      [[maybe_unused]] const char *name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ---- outputs and globals --------------------------------------------------------- */

static void out_geometry([[maybe_unused]] void *data,
                         [[maybe_unused]] struct wl_output *wayland_output,
                         [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                         [[maybe_unused]] int32_t pw, [[maybe_unused]] int32_t ph,
                         [[maybe_unused]] int32_t sub, [[maybe_unused]] const char *make,
                         [[maybe_unused]] const char *model, [[maybe_unused]] int32_t transform) {}
static void out_mode([[maybe_unused]] void *data, [[maybe_unused]] struct wl_output *wayland_output,
                     [[maybe_unused]] uint32_t flags, [[maybe_unused]] int32_t width,
                     [[maybe_unused]] int32_t height, [[maybe_unused]] int32_t refresh) {}
static void out_done([[maybe_unused]] void *data,
                     [[maybe_unused]] struct wl_output *wayland_output) {}
static void out_scale(void *data, [[maybe_unused]] struct wl_output *wo, int32_t factor) {
    output *screen_output = data;
    screen_output->int_scale = factor;
    screen_output->dirty = true;
}
static void out_name([[maybe_unused]] void *data, [[maybe_unused]] struct wl_output *wayland_output,
                     [[maybe_unused]] const char *name) {}
static void out_description([[maybe_unused]] void *data,
                            [[maybe_unused]] struct wl_output *wayland_output,
                            [[maybe_unused]] const char *description) {}

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
    app *locker = data;
#define IS(i) (strcmp(iface, (i).name) == 0)
#define BIND(i, v) wl_registry_bind(reg, name, &(i), XD_MIN(version, (uint32_t)(v)))
    if (IS(wl_compositor_interface)) {
        locker->compositor = BIND(wl_compositor_interface, 4);
    } else if (IS(wl_shm_interface)) {
        locker->shm = BIND(wl_shm_interface, 1);
    } else if (IS(wl_seat_interface) && locker->seat == nullptr) {
        locker->seat = BIND(wl_seat_interface, 5);
        wl_seat_add_listener(locker->seat, &seat_listener, locker);
    } else if (IS(ext_session_lock_manager_v1_interface)) {
        locker->lock_manager = BIND(ext_session_lock_manager_v1_interface, 1);
    } else if (IS(zwlr_layer_shell_v1_interface)) {
        locker->layer_shell = BIND(zwlr_layer_shell_v1_interface, 4);
    } else if (IS(wp_viewporter_interface)) {
        locker->viewporter = BIND(wp_viewporter_interface, 1);
    } else if (IS(wp_fractional_scale_manager_v1_interface)) {
        locker->fractional_manager = BIND(wp_fractional_scale_manager_v1_interface, 1);
    } else if (IS(wp_cursor_shape_manager_v1_interface)) {
        locker->cursor_shape_manager = BIND(wp_cursor_shape_manager_v1_interface, 1);
    } else if (IS(wl_output_interface)) {
        output *screen_output = XD_NEW(output);
        screen_output->app = locker;
        screen_output->global = name;
        screen_output->int_scale = 1;
        screen_output->wl_output = BIND(wl_output_interface, 4);
        wl_output_add_listener(screen_output->wl_output, &output_listener, screen_output);
        wl_list_insert(locker->outputs.prev, &screen_output->link);
        /* A monitor plugged in while locked gets covered too. */
        create_surface(locker, screen_output);
    }
#undef BIND
#undef IS
}

static void registry_global_remove(void *data, [[maybe_unused]] struct wl_registry *reg,
                                   uint32_t name) {
    app *locker = data;
    XD_LIST_FOR_EACH_SAFE(screen_output, tmp, &locker->outputs, output, link) {
        if (screen_output->global == name)
            destroy_output(screen_output);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---- main --------------------------------------------------------------------- */

static void who_am_i(app *locker) {
    struct passwd *pw = getpwuid(getuid());
    snprintf(locker->user, sizeof(locker->user), "%s", pw != nullptr ? pw->pw_name : "user");
    char host[256] = "localhost";
    gethostname(host, sizeof(host) - 1);
    char full[256] = "";
    if (pw != nullptr && pw->pw_gecos != nullptr) {
        snprintf(full, sizeof(full), "%s", pw->pw_gecos);
        char *comma = strchr(full, ',');
        if (comma != nullptr)
            *comma = '\0';
    }
    if (full[0] != '\0' && strcmp(full, locker->user) != 0)
        snprintf(locker->who, sizeof(locker->who), "%s@%s (%s)", locker->user, host, full);
    else
        snprintf(locker->who, sizeof(locker->who), "%s@%s", locker->user, host);
}

static bool flush(struct wl_display *display) {
    while (wl_display_flush(display) < 0) {
        if (errno != EAGAIN)
            return false;
        struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLOUT};
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR)
            return false;
    }
    return true;
}

static void run(app *locker) {
    struct wl_display *display = locker->display;
    while (locker->running) {
        XD_LIST_FOR_EACH(screen_output, &locker->outputs, output, link) {
            if (screen_output->configured && screen_output->dirty)
                render(screen_output);
        }
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        if (!flush(display)) {
            wl_display_cancel_read(display);
            break;
        }
        struct pollfd fds[5] = {
            {.fd = wl_display_get_fd(display), .events = POLLIN},
            {.fd = locker->pam_pipe[0], .events = POLLIN},
            {.fd = locker->idle_fd, .events = POLLIN},
            {.fd = locker->frame_fd, .events = POLLIN},
            {.fd = locker->logind.pipe[0], .events = POLLIN}, /* -1 without logind: ignored */
        };
        if (poll(fds, 5, -1) < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR)
                continue;
            break;
        }
        bool ok = true;
        if (fds[0].revents & POLLIN)
            ok = wl_display_read_events(display) >= 0;
        else
            wl_display_cancel_read(display);
        if (!ok || (fds[0].revents & (POLLERR | POLLHUP)) ||
            wl_display_dispatch_pending(display) < 0) {
            fprintf(stderr, "xd-lock: lost the compositor\n");
            locker->exit_code = EXIT_FAILURE;
            break;
        }
        if (fds[1].revents & POLLIN)
            check_result(locker);
        if (fds[2].revents & POLLIN) {
            uint64_t expirations;
            (void)!read(locker->idle_fd, &expirations, sizeof(expirations));
            if (locker->screen != SCREEN_CHECKING && locker->saver_id[0] != '\0')
                go(locker, SCREEN_SAVER);
        }
        if (fds[3].revents & POLLIN) {
            uint64_t expirations;
            (void)!read(locker->frame_fd, &expirations, sizeof(expirations));
            XD_LIST_FOR_EACH(screen_output, &locker->outputs, output, link) {
                if (screen_output->throttled && locker->screen == SCREEN_SAVER)
                    screen_output->dirty = true;
                screen_output->throttled = false;
            }
        }
        if (fds[4].revents & POLLIN)
            logind_message(locker);
    }
    flush(display);
}

static void usage(void) {
    fprintf(stderr, "usage: xd-lock [--saver [ID] [--lock|--no-lock]]\n");
}

int main(int argc, char **argv) {
    /* The password passes through this process: keep it out of core
     * dumps and swap. */
    prctl(PR_SET_DUMPABLE, 0);

    static app locker;
    wl_list_init(&locker.outputs);
    locker.exit_code = EXIT_SUCCESS;
    xd_settings_load(&locker.settings);
    xd_settings_scheme(&locker.settings, &locker.scheme);
    xd_pkg_free(xd_settings_use_package(&locker.settings)); /* its unlock box, as the desktop's */

    bool saver = false;
    int lock_opt = -1; /* -1: from settings */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--saver") == 0) {
            saver = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                snprintf(locker.saver_id, sizeof(locker.saver_id), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--lock") == 0) {
            lock_opt = 1;
        } else if (strcmp(argv[i], "--no-lock") == 0) {
            lock_opt = 0;
        } else if (strcmp(argv[i], "--style") == 0 && i + 1 < argc) {
            /* The shell passes on its own overrides. */
            if (!xd_visual_style_from_name(xd_str_from_cstr(argv[++i]), &locker.settings.style))
                fprintf(stderr, "xd-lock: unknown style %s\n", argv[i]);
        } else if (strcmp(argv[i], "--scheme") == 0 && i + 1 < argc) {
            xd_strbuf err = {0};
            if (!xd_scheme_load(argv[++i], &locker.scheme, &err))
                fprintf(stderr, "xd-lock: %s: %s\n", argv[i], xd_strbuf_cstr(&err));
            xd_strbuf_free(&err);
        } else {
            usage();
            return EXIT_FAILURE;
        }
    }
    if (saver && locker.saver_id[0] == '\0')
        snprintf(locker.saver_id, sizeof(locker.saver_id), "%s", locker.settings.screensaver);
    if (saver && xd_saver_find(locker.saver_id) == nullptr) {
        fprintf(stderr, "xd-lock: no screen saver \"%s\"\n", locker.saver_id);
        return EXIT_FAILURE;
    }
    if (saver) {
        locker.locking = lock_opt >= 0 ? lock_opt == 1 : locker.settings.screensaver_password;
        locker.screen = SCREEN_SAVER;
        locker.idle_seconds = 30; /* an abandoned unlock dialog gives way to the saver */
    } else {
        locker.locking = true;
        locker.screen = SCREEN_LOCKED;
        /* A locked computer runs the screen saver after its usual wait. */
        if (xd_saver_find(locker.settings.screensaver) != nullptr) {
            snprintf(locker.saver_id, sizeof(locker.saver_id), "%s", locker.settings.screensaver);
            locker.idle_seconds = XD_MAX(locker.settings.screensaver_wait, 1) * 60;
        }
    }
    who_am_i(&locker);

    locker.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    locker.display = wl_display_connect(nullptr);
    if (locker.display == nullptr) {
        fprintf(stderr, "xd-lock: cannot connect to a Wayland compositor\n");
        return EXIT_FAILURE;
    }
    struct wl_registry *reg = wl_display_get_registry(locker.display);
    wl_registry_add_listener(reg, &registry_listener, &locker);
    wl_display_roundtrip(locker.display);
    if (locker.compositor == nullptr || locker.shm == nullptr) {
        fprintf(stderr, "xd-lock: compositor lacks wl_shm\n");
        return EXIT_FAILURE;
    }
    if (locker.locking && locker.lock_manager == nullptr) {
        fprintf(stderr, "xd-lock: the compositor can't lock the screen (ext-session-lock)\n");
        return EXIT_FAILURE;
    }
    if (!locker.locking && locker.layer_shell == nullptr) {
        fprintf(stderr, "xd-lock: the compositor lacks layer-shell\n");
        return EXIT_FAILURE;
    }
    if (pipe2(locker.pam_pipe, O_CLOEXEC) < 0) {
        perror("xd-lock: pipe");
        return EXIT_FAILURE;
    }
    locker.idle_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    locker.frame_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    locker.icons = xd_icons_create(xd_settings_icon_theme(&locker.settings));

    if (locker.locking) {
        locker.lock = ext_session_lock_manager_v1_lock(locker.lock_manager);
        ext_session_lock_v1_add_listener(locker.lock, &lock_listener, &locker);
    }
    XD_LIST_FOR_EACH(screen_output, &locker.outputs, output, link) {
        create_surface(&locker, screen_output);
    }
    locker.running = true;
    logind_start(&locker);
    arm_idle(&locker);
    run(&locker);
    atomic_store(&locker.logind.unlocked, locker.lock == nullptr);
    logind_stop(&locker);

    if (locker.pam_running)
        pthread_join(locker.pam_thread, nullptr);
    explicit_bzero(locker.password, sizeof(locker.password));
    XD_LIST_FOR_EACH_SAFE(screen_output, tmp, &locker.outputs, output, link) {
        destroy_output(screen_output);
    }
    /* Still locked (lost the compositor, or it ended the lock): just
     * disconnect. Destroying a held lock would be a protocol error, and
     * the compositor keeps the screen locked when we go. */
    if (locker.lock != nullptr && !locker.locked)
        ext_session_lock_v1_destroy(locker.lock);
    xd_icons_destroy(locker.icons);
    xkb_state_unref(locker.xkb_state);
    xkb_keymap_unref(locker.keymap);
    xkb_context_unref(locker.xkb);
    close(locker.idle_fd);
    close(locker.frame_fd);
    close(locker.pam_pipe[0]);
    close(locker.pam_pipe[1]);
    wl_display_disconnect(locker.display);
    return locker.exit_code;
}
