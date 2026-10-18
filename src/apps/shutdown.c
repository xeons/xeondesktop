/* SPDX-License-Identifier: MIT
 * xd-shutdown: the Shut Down dialog, and with --logoff the Log Off
 * confirmation: in Luna XP's "Log Off" box with Fast User Switching (Switch
 * User and Log Off), otherwise Windows 2000's "Are you sure you want to log
 * off?".
 *
 * Power actions go through systemctl (logind decides via polkit whether
 * the user may). In a nested development session they are only printed,
 * so a test click can't power off the host.
 *
 * Log Off ends our compositor, which ends the session. When the session
 * isn't our compositor's (the taskbar alone under another compositor),
 * logind's TerminateSession ends the caller's session instead. Switch User
 * goes to the display manager's greeter as the Start menu's does
 * (src/shell/account.c), the screen locked first; xd-lock unlocks by
 * itself once logind says the greeter let the user back in.
 */
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "../settings/login1.h"
#include "../settings/soundplay.h"
#include "../ui/ui.h"
#include "common.h"

#define FDO_DM "org.freedesktop.DisplayManager"
#define FDO_DM_PATH "/org/freedesktop/DisplayManager"
#define FDO_DM_SEAT "org.freedesktop.DisplayManager.Seat"
#define GDM "org.gnome.DisplayManager"
#define GDM_FACTORY_PATH "/org/gnome/DisplayManager/LocalDisplayFactory"
#define GDM_FACTORY "org.gnome.DisplayManager.LocalDisplayFactory"

/* How long the locker gets to cover the screens before the greeter (the
 * Start menu waits as long). */
static constexpr useconds_t LOCK_GRACE_US = 1000 * 1000;

/* --dry-run: Log Off and Switch User only print what they'd ask of logind
 * or the display manager. */
static bool dry_run;

typedef enum action : uint8_t {
    ACT_LOGOFF,
    ACT_SHUTDOWN,
    ACT_RESTART,
    ACT_STANDBY,
    ACT_HIBERNATE,
} action;

static const struct {
    const char *key; /* saved as the last choice */
    const char *label;
    const char *description;
    const char *systemctl;
} actions[] = {
    [ACT_LOGOFF] = {"logoff", "Log off %s",
                    "Ends your session. The computer keeps running so that someone else can "
                    "log on.",
                    nullptr},
    [ACT_SHUTDOWN] = {"shutdown", "Shut down",
                      "Ends your session and shuts down the computer so that you can safely "
                      "turn off the power.",
                      "poweroff"},
    [ACT_RESTART] = {"restart", "Restart",
                     "Ends your session, shuts down the computer, and starts it again.",
                     "reboot"},
    [ACT_STANDBY] = {"standby", "Stand by",
                     "Keeps your session in memory and puts the computer in a low-power state. "
                     "Move the mouse or press a key to wake it.",
                     "suspend"},
    [ACT_HIBERNATE] = {"hibernate", "Hibernate",
                       "Saves your session to disk and turns off the computer. Your session is "
                       "restored the next time you start it.",
                       "hibernate"},
};

/* The XP Log Off box's two buttons. */
typedef enum tile : uint8_t { TILE_SWITCH, TILE_LOGOFF, TILE_COUNT } tile;

typedef struct dialog {
    xd_ui *ui;
    xd_window *win;
    xd_widget *combo;
    xd_widget *description;
    action items[5]; /* combo index -> action */
    size_t count;
    bool simulate;
    xd_widget *tiles[TILE_COUNT];
    bool tile_enabled[TILE_COUNT];
    int pressed; /* the tile a held button went down on, or -1 */
} dialog;

/* Is the session leader really our compositor? Guards against a stale or
 * forged XEONDESKTOP_PID making Log Off kill something else. */
static pid_t compositor_pid(void) {
    const char *env = getenv("XEONDESKTOP_PID");
    int64_t pid;
    if (env == nullptr || !xd_str_parse_i64(xd_str_from_cstr(env), 2, INT32_MAX, &pid))
        return 0;
    xd_strbuf path = {0};
    xd_strbuf comm = {0};
    xd_strbuf_appendf(&path, "/proc/%lld/comm", (long long)pid);
    bool ok = false;
    if (xd_read_file(xd_strbuf_cstr(&path), 256, &comm)) {
        /* xd-compositor, or xd-compositor-fx (the GPU build it hands over
         * to), which the kernel cuts to 15 characters: "xd-compositor-f". */
        xd_str name = xd_str_trim(xd_strbuf_view(&comm));
        ok = xd_str_eq(name, XD_STR("xd-compositor")) || xd_str_eq(name, XD_STR("xd-compositor-f"));
    }
    xd_strbuf_free(&comm);
    xd_strbuf_free(&path);
    return ok ? (pid_t)pid : 0;
}

/* ---- logind and the display manager ---- */

/* The system bus, or nullptr in a nested or test session without a mock
 * one: then nothing is asked of the system. */
static GDBusConnection *system_bus(void) {
    if (xd_login1_simulated())
        return nullptr;
    GError *error = nullptr;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (bus == nullptr) {
        fprintf(stderr, "xd-shutdown: no system bus: %s\n", error->message);
        g_error_free(error);
    }
    return bus;
}

/* Log Off when our compositor isn't the session's: logind ends the
 * caller's session (polkit lets a user end their own). */
static bool terminate_session(void) {
    if (dry_run || xd_login1_simulated()) {
        fprintf(stderr, "xd-shutdown: nested session, not running: logind TerminateSession "
                        "for this session\n");
        return true;
    }
    GDBusConnection *bus = system_bus();
    if (bus == nullptr)
        return false;
    GError *error = nullptr;
    char *id = nullptr;
    char *path = xd_login1_own_session(bus, nullptr, &error);
    if (path != nullptr)
        id = xd_login1_session_string(bus, path, "Id", nullptr, &error);
    GVariant *r = nullptr;
    if (id != nullptr)
        r = g_dbus_connection_call_sync(bus, XD_LOGIN1, XD_LOGIN1_PATH, XD_LOGIN1_MANAGER,
                                        "TerminateSession", g_variant_new("(s)", id), nullptr,
                                        G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1,
                                        nullptr, &error);
    bool ok = r != nullptr;
    if (ok) {
        g_variant_unref(r);
    } else {
        fprintf(stderr, "xd-shutdown: cannot end the session: %s\n", error->message);
        g_error_free(error);
    }
    g_free(id);
    g_free(path);
    g_object_unref(bus);
    return ok;
}

static bool name_has_owner(GDBusConnection *bus, const char *name) {
    GVariant *r = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "NameHasOwner", g_variant_new("(s)", name), G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    gboolean owned = FALSE;
    if (r != nullptr) {
        g_variant_get(r, "(b)", &owned);
        g_variant_unref(r);
    }
    return owned;
}

/* LightDM and SDDM offer org.freedesktop.DisplayManager, GDM its own. */
static bool can_switch_user(void) {
    GDBusConnection *bus = system_bus();
    if (bus == nullptr)
        return false;
    bool can = name_has_owner(bus, FDO_DM) || name_has_owner(bus, GDM);
    g_object_unref(bus);
    return can;
}

/* The session's seat (XDG_SEAT_PATH, set by LightDM and SDDM), else the
 * display manager's first. */
static char *dm_seat(GDBusConnection *bus, GError **error) {
    const char *seat = getenv("XDG_SEAT_PATH");
    if (seat != nullptr && g_variant_is_object_path(seat))
        return g_strdup(seat);
    GVariant *r = g_dbus_connection_call_sync(
        bus, FDO_DM, FDO_DM_PATH, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", FDO_DM, "Seats"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1,
        nullptr, error);
    if (r == nullptr)
        return nullptr;
    GVariant *v = nullptr;
    g_variant_get(r, "(v)", &v);
    char *path = nullptr;
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH_ARRAY) && g_variant_n_children(v) > 0)
        g_variant_get_child(v, 0, "o", &path);
    else
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "the display manager has no seats");
    g_variant_unref(v);
    g_variant_unref(r);
    return path;
}

static bool switch_user(void) {
    if (dry_run || xd_login1_simulated()) {
        fprintf(stderr, "xd-shutdown: nested session, not running: lock and switch to the "
                        "greeter\n");
        return true;
    }
    GDBusConnection *bus = system_bus();
    if (bus == nullptr)
        return false;
    bool fdo = name_has_owner(bus, FDO_DM);
    if (!fdo && !name_has_owner(bus, GDM)) {
        fprintf(stderr, "xd-shutdown: cannot switch user: no display manager\n");
        g_object_unref(bus);
        return false;
    }
    /* Locked first, as GNOME and KDE do: the session stays on its own VT,
     * and switching back to that by hand mustn't skip the password. */
    if (xd_spawn_own("xd-lock", nullptr, nullptr) == 0)
        fprintf(stderr, "xd-shutdown: cannot start xd-lock\n");
    else
        usleep(LOCK_GRACE_US);
    GError *error = nullptr;
    GVariant *r = nullptr;
    if (fdo) {
        char *seat = dm_seat(bus, &error);
        if (seat != nullptr)
            r = g_dbus_connection_call_sync(bus, FDO_DM, seat, FDO_DM_SEAT, "SwitchToGreeter",
                                            nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                                            &error);
        g_free(seat);
    } else {
        r = g_dbus_connection_call_sync(bus, GDM, GDM_FACTORY_PATH, GDM_FACTORY,
                                        "CreateTransientDisplay", nullptr, nullptr,
                                        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    }
    bool ok = r != nullptr;
    if (ok) {
        g_variant_unref(r);
    } else {
        fprintf(stderr, "xd-shutdown: cannot switch user: %s\n",
                error != nullptr ? error->message : "no display manager seat");
        g_clear_error(&error);
    }
    g_object_unref(bus);
    return ok;
}

static bool perform(action a, bool simulate) {
    /* Windows Logoff or Exit Windows, played out before the session ends. */
    if (a == ACT_LOGOFF || a == ACT_SHUTDOWN || a == ACT_RESTART)
        xd_sound_play_event_wait(a == ACT_LOGOFF ? XD_SOUND_LOGOFF : XD_SOUND_EXIT, 5000);
    if (a == ACT_LOGOFF) {
        pid_t pid = compositor_pid();
        if (pid == 0)
            return terminate_session();
        return kill(pid, SIGTERM) == 0;
    }
    if (simulate) {
        fprintf(stderr, "xd-shutdown: nested session, not running: systemctl %s\n",
                actions[a].systemctl);
        return true;
    }
    char prog[] = "systemctl";
    char *verb = xd_str_dup_cstr(xd_str_from_cstr(actions[a].systemctl));
    char *argv[] = {prog, verb, nullptr};
    xd_strbuf err = {0};
    bool ok = xd_launch_argv(argv, &err);
    if (!ok)
        fprintf(stderr, "xd-shutdown: systemctl %s: %s\n", verb, xd_strbuf_cstr(&err));
    xd_strbuf_free(&err);
    xd_free(verb);
    return ok;
}

/* Which sleep states the kernel offers ("freeze mem disk"). */
static void sleep_states(bool *standby, bool *hibernate) {
    xd_strbuf s = {0};
    *standby = false;
    *hibernate = false;
    if (xd_read_file("/sys/power/state", 4096, &s)) {
        xd_str rest = xd_str_trim(xd_strbuf_view(&s));
        while (rest.len > 0) {
            size_t sp = rest.len;
            bool more = xd_str_find_char(rest, ' ', &sp);
            xd_str word = xd_str_sub(rest, 0, sp);
            *standby = *standby || xd_str_eq(word, XD_STR("mem"));
            *hibernate = *hibernate || xd_str_eq(word, XD_STR("disk"));
            rest = more ? xd_str_sub(rest, sp + 1, rest.len) : XD_STR("");
        }
    }
    xd_strbuf_free(&s);
}

static void on_change(xd_widget *combo, void *user) {
    dialog *d = user;
    int i = xd_combo_selected(combo);
    if (i >= 0 && (size_t)i < d->count)
        xd_widget_set_text(d->description, actions[d->items[i]].description);
}

static void on_ok([[maybe_unused]] xd_widget *w, void *user) {
    dialog *d = user;
    int i = xd_combo_selected(d->combo);
    if (i < 0 || (size_t)i >= d->count)
        return;
    action a = d->items[i];
    char *key = xd_str_dup_cstr(xd_str_from_cstr(actions[a].key));
    if (!xd_state_write_lines("shutdown-choice", &key, 1))
        fprintf(stderr, "xd-shutdown: cannot save the last choice\n");
    xd_free(key);
    xd_window_close(d->win);
    perform(a, d->simulate);
}

static void on_cancel([[maybe_unused]] xd_widget *w, void *user) {
    dialog *d = user;
    xd_window_close(d->win);
}

static void build_shutdown(dialog *d) {
    d->win = xd_window_create(d->ui, "Shut Down XeonDesktop", "xd-shutdown", 412, 208);
    xd_banner(d->win, (xd_rect){0, 0, 412, 72}, "XeonDesktop", "Professional");
    xd_image(d->win, (xd_rect){12, 86, 32, 32}, "system-shutdown");
    xd_label(d->win, (xd_rect){56, 86, 344, 16}, "What do you want the computer to do?", 0);
    d->combo = xd_combo(d->win, (xd_rect){56, 106, 220, 21}, false, on_change, d);
    d->description = xd_label(d->win, (xd_rect){56, 134, 344, 32}, "", XD_LABEL_WRAP);

    bool standby;
    bool hibernate;
    sleep_states(&standby, &hibernate);
    const char *user = getenv("USER");
    for (size_t a = 0; a < XD_ARRAY_LEN(actions); a++) {
        if ((a == ACT_STANDBY && !standby) || (a == ACT_HIBERNATE && !hibernate))
            continue;
        xd_strbuf label = {0};
        if (a == ACT_LOGOFF)
            xd_strbuf_appendf(&label, "Log off %s", user != nullptr ? user : "");
        else
            xd_strbuf_append(&label, xd_str_from_cstr(actions[a].label));
        xd_combo_add(d->combo, xd_strbuf_cstr(&label));
        xd_strbuf_free(&label);
        d->items[d->count++] = (action)a;
    }

    /* Start on the last choice, like Windows; Shut down otherwise. */
    char **last = nullptr;
    size_t n = 0;
    xd_state_read_lines("shutdown-choice", &last, &n, 1);
    int pick = 0;
    for (size_t i = 0; i < d->count; i++) {
        if (n > 0 ? strcmp(actions[d->items[i]].key, last[0]) == 0 : d->items[i] == ACT_SHUTDOWN)
            pick = (int)i;
    }
    xd_free_lines(last, n);
    xd_combo_select(d->combo, pick);
    on_change(d->combo, d);

    if (d->simulate)
        xd_label(d->win, (xd_rect){12, 177, 140, 16}, "Test session: simulated",
                 XD_LABEL_DIM);
    xd_widget *ok = xd_button(d->win, (xd_rect){163, 173, 75, 23}, "OK", on_ok, d);
    xd_widget *cancel = xd_button(d->win, (xd_rect){244, 173, 75, 23}, "Cancel", on_cancel, d);
    xd_widget *help = xd_button(d->win, (xd_rect){325, 173, 75, 23}, "Help", nullptr, nullptr);
    xd_widget_set_enabled(help, false);
    xd_window_set_default(d->win, ok);
    xd_window_set_cancel(d->win, cancel);
    xd_window_set_focus(d->win, d->combo);
}

static void on_logoff_yes([[maybe_unused]] xd_widget *w, void *user) {
    dialog *d = user;
    xd_window_close(d->win);
    perform(ACT_LOGOFF, d->simulate);
}

/* Windows 2000's "Are you sure you want to log off?" box. */
static void build_logoff(dialog *d) {
    d->win = xd_window_create(d->ui, "Log Off XeonDesktop", "xd-shutdown", 300, 100);
    xd_image(d->win, (xd_rect){12, 14, 32, 32}, "system-log-out");
    xd_label(d->win, (xd_rect){56, 22, 232, 16}, "Are you sure you want to log off?", 0);
    xd_widget *yes = xd_button(d->win, (xd_rect){72, 62, 75, 23}, "Yes", on_logoff_yes, d);
    xd_widget *no = xd_button(d->win, (xd_rect){153, 62, 75, 23}, "No", on_cancel, d);
    xd_window_set_default(d->win, yes);
    xd_window_set_cancel(d->win, no);
    xd_window_set_focus(d->win, yes);
}

/* ---- XP's Log Off box ---- */

/* XP's layout: a dark blue band with the title and logo, a lighter blue
 * band with the two buttons, and a dark blue band with Cancel. */
enum {
    XP_W = 314,
    XP_H = 198,
    XP_TOP_H = 48,
    XP_BOTTOM_Y = 150,
    TILE_W = 90,
    TILE_H = 70,
    TILE_Y = 64,
    TILE_ICON = 32,
};
static const int tile_x[TILE_COUNT] = {[TILE_SWITCH] = 62, [TILE_LOGOFF] = 162};
static const char *const tile_label[TILE_COUNT] = {[TILE_SWITCH] = "Switch User",
                                                   [TILE_LOGOFF] = "Log Off"};

static constexpr uint32_t XP_DARK = 0x00309C;
static constexpr uint32_t XP_LIGHT = 0x5A7EDC;

static void draw_top_band(xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t,
                          [[maybe_unused]] void *user) {
    (void)w;
    xd_fill_rect(cr, r, xd_rgba_hex(XP_DARK));
    /* The bright rule under the band, fading out at both ends. */
    int rule = XD_MAX(xd_tpx(t, 2), 1);
    xd_rect line = {r.x, r.y + r.h - rule, r.w, rule};
    cairo_pattern_t *g = cairo_pattern_create_linear(r.x, 0, r.x + r.w, 0);
    cairo_pattern_add_color_stop_rgb(g, 0, 0x5A / 255.0, 0x7E / 255.0, 0xDC / 255.0);
    cairo_pattern_add_color_stop_rgb(g, 0.35, 1, 1, 1);
    cairo_pattern_add_color_stop_rgb(g, 1, 0x5A / 255.0, 0x7E / 255.0, 0xDC / 255.0);
    cairo_rectangle(cr, line.x, line.y, line.w, line.h);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    xd_font_spec font = {.size_pt = 14};
    snprintf(font.family, sizeof(font.family), "%s", XD_FONT_FAMILY_FRANKLIN);
    xd_text_style st = xd_text(t, XD_FONT_MESSAGE, xd_rgba_hex(0xFFFFFF), XD_ALIGN_START);
    st.font = &font;
    xd_draw_text(cr, (xd_rect){r.x + xd_tpx(t, 11), r.y, r.w - xd_tpx(t, 60), r.h - rule},
                 "Log Off XeonDesktop", &st);
    int logo = xd_tpx(t, 30);
    cairo_new_path(cr); /* the text left a current point */
    xd_draw_start_logo(cr, (xd_rect){r.x + r.w - logo - xd_tpx(t, 10),
                                     r.y + (r.h - rule - logo) / 2, logo, logo});
}

static void draw_band(xd_widget *w, cairo_t *cr, xd_rect r, [[maybe_unused]] const xd_theme *t,
                      void *user) {
    (void)w;
    xd_fill_rect(cr, r, xd_rgba_hex((uint32_t)(uintptr_t)user));
}

/* Rounded tile with a white edge and a vertical gradient. */
static void tile_base(cairo_t *cr, const xd_theme *t, xd_rect r, uint32_t top, uint32_t bottom) {
    double rad = xd_tpx(t, 3);
    double lw = XD_MAX(xd_tpx(t, 1), 1);
    cairo_new_path(cr);
    xd_path_rounded(cr, r.x + lw / 2, r.y + lw / 2, r.w - lw, r.h - lw, rad, rad, rad, rad);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
    xd_rgba a = xd_rgba_hex(top);
    xd_rgba b = xd_rgba_hex(bottom);
    cairo_pattern_add_color_stop_rgb(g, 0, a.r, a.g, a.b);
    cairo_pattern_add_color_stop_rgb(g, 1, b.r, b.g, b.b);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    xd_set_color(cr, xd_rgba_hex(0xFFFFFF));
    cairo_set_line_width(cr, lw);
    cairo_stroke(cr);
}

/* A head and shoulders. */
static void person(cairo_t *cr, double cx, double cy, double u) {
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy - 3 * u, 2.6 * u, 0, 2 * XD_PI);
    cairo_fill(cr);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy + 5.5 * u, 5 * u, XD_PI, 2 * XD_PI);
    cairo_close_path(cr);
    cairo_fill(cr);
}

/* Switch User: a green tile with two people; Log Off: the Start menu's
 * orange tile with a padlock. Greyed out, the tile loses its colour. */
static void draw_tile_icon(cairo_t *cr, const xd_theme *t, xd_rect r, tile which, bool enabled) {
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0, u = r.w / 22.0;
    if (which == TILE_SWITCH) {
        tile_base(cr, t, r, enabled ? 0x8CD36A : 0xC8C8C8, enabled ? 0x2E8B1E : 0x8C8C8C);
        xd_set_color(cr, xd_rgba_hex(enabled ? 0xBFE8A8 : 0xE0E0E0));
        person(cr, cx + 3 * u, cy - u, u * 0.9);
        xd_set_color(cr, xd_rgba_hex(0xFFFFFF));
        person(cr, cx - 2.5 * u, cy + 0.5 * u, u);
        return;
    }
    uint32_t bottom = enabled ? 0xD9780A : 0x8C8C8C;
    tile_base(cr, t, r, enabled ? 0xF9C15A : 0xC8C8C8, bottom);
    xd_set_color(cr, xd_rgba_hex(0xFFFFFF));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, 2 * u);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy - 2 * u, 4 * u, XD_PI, 2 * XD_PI);
    cairo_stroke(cr);
    cairo_rectangle(cr, cx - 6 * u, cy - u, 12 * u, 9 * u);
    cairo_fill(cr);
    xd_set_color(cr, xd_rgba_hex(bottom));
    cairo_rectangle(cr, cx - u, cy + 2 * u, 2 * u, 3 * u);
    cairo_fill(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

static tile tile_of(const dialog *d, const xd_widget *w) {
    return w == d->tiles[TILE_SWITCH] ? TILE_SWITCH : TILE_LOGOFF;
}

static void draw_tile(xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t, void *user) {
    dialog *d = user;
    tile which = tile_of(d, w);
    bool enabled = d->tile_enabled[which];
    xd_fill_rect(cr, r, xd_rgba_hex(XP_LIGHT));
    int icon = xd_tpx(t, TILE_ICON);
    int down = d->pressed == (int)which ? xd_tpx(t, 1) : 0;
    xd_rect ir = {r.x + (r.w - icon) / 2 + down, r.y + xd_tpx(t, 4) + down, icon, icon};
    draw_tile_icon(cr, t, ir, which, enabled);
    xd_text_style st = xd_text(t, XD_FONT_MESSAGE,
                               xd_rgba_hex(enabled ? 0xFFFFFF : 0xA9BCEC), XD_ALIGN_CENTER);
    xd_draw_text(cr, (xd_rect){r.x, ir.y + icon + xd_tpx(t, 6), r.w, xd_tpx(t, 16)},
                 tile_label[which], &st);
}

static void tile_pointer(xd_widget *w, xd_pointer_event ev, double x, double y, void *user) {
    dialog *d = user;
    tile which = tile_of(d, w);
    if (!d->tile_enabled[which])
        return;
    bool inside = x >= 0 && y >= 0 && x < (double)TILE_W && y < (double)TILE_H;
    int was = d->pressed;
    if (ev == XD_POINTER_PRESS || ev == XD_POINTER_MOTION)
        d->pressed = inside ? (int)which : -1;
    else
        d->pressed = -1;
    if (d->pressed != was)
        xd_widget_redraw(w);
    if (ev != XD_POINTER_RELEASE || !inside)
        return;
    xd_window_close(d->win);
    if (which == TILE_SWITCH)
        switch_user();
    else
        perform(ACT_LOGOFF, d->simulate);
}

/* XP's "Log Off Windows" with Fast User Switching on: Switch User keeps
 * this session running and goes to the Welcome screen (the display
 * manager's greeter), Log Off ends it. */
static void build_logoff_xp(dialog *d) {
    d->win = xd_window_create(d->ui, "Log Off XeonDesktop", "xd-shutdown", XP_W, XP_H);
    d->pressed = -1;
    xd_custom(d->win, (xd_rect){0, 0, XP_W, XP_TOP_H}, draw_top_band, d);
    xd_custom(d->win, (xd_rect){0, XP_TOP_H, XP_W, XP_BOTTOM_Y - XP_TOP_H}, draw_band,
              (void *)(uintptr_t)XP_LIGHT);
    xd_custom(d->win, (xd_rect){0, XP_BOTTOM_Y, XP_W, XP_H - XP_BOTTOM_Y}, draw_band,
              (void *)(uintptr_t)XP_DARK);
    d->tile_enabled[TILE_SWITCH] = can_switch_user();
    d->tile_enabled[TILE_LOGOFF] = true;
    for (int i = 0; i < TILE_COUNT; i++) {
        d->tiles[i] = xd_custom(d->win, (xd_rect){tile_x[i], TILE_Y, TILE_W, TILE_H}, draw_tile, d);
        xd_custom_set_pointer(d->tiles[i], tile_pointer);
    }
    xd_widget *cancel = xd_button(d->win, (xd_rect){XP_W - 75 - 12, XP_BOTTOM_Y + 13, 75, 23},
                                  "Cancel", on_cancel, d);
    xd_window_set_default(d->win, cancel);
    xd_window_set_cancel(d->win, cancel);
    xd_window_set_focus(d->win, cancel);
}

int main(int argc, char **argv) {
    bool logoff = false;
    const char *direct = nullptr; /* --action: act without a dialog */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--logoff") == 0)
            logoff = true;
        else if (strcmp(argv[i], "--dry-run") == 0)
            dry_run = true;
        else if (strcmp(argv[i], "--action") == 0 && i + 1 < argc)
            direct = argv[++i];
    }

    dialog d = {0};
    const char *nested = getenv("XEONDESKTOP_NESTED");
    d.simulate = dry_run || nested == nullptr || strcmp(nested, "0") != 0;

    /* Windows 7's Shut Down button acts at once; the same safety rules
     * (nested sessions only simulate) apply. */
    if (direct != nullptr) {
        /* The Task Manager's Disconnect on our own session. */
        if (strcmp(direct, "switchuser") == 0)
            return switch_user() ? EXIT_SUCCESS : EXIT_FAILURE;
        for (size_t a = 0; a < XD_ARRAY_LEN(actions); a++) {
            if (strcmp(actions[a].key, direct) == 0)
                return perform((action)a, d.simulate) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        fprintf(stderr, "xd-shutdown: unknown action %s\n", direct);
        return EXIT_FAILURE;
    }
    d.ui = xd_ui_connect(argc, argv);
    if (d.ui == nullptr)
        return EXIT_FAILURE;
    if (logoff && xd_ui_style(d.ui) == XD_STYLE_LUNA)
        build_logoff_xp(&d);
    else if (logoff)
        build_logoff(&d);
    else
        build_shutdown(&d);
    xd_window_show(d.win);
    xd_ui_run(d.ui);
    xd_ui_destroy(d.ui);
    return EXIT_SUCCESS;
}
