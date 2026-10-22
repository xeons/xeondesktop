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
    GVariant *variant = nullptr;
    if (id != nullptr)
        variant = g_dbus_connection_call_sync(bus, XD_LOGIN1, XD_LOGIN1_PATH, XD_LOGIN1_MANAGER,
                                              "TerminateSession", g_variant_new("(s)", id), nullptr,
                                              G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1,
                                              nullptr, &error);
    bool ok = variant != nullptr;
    if (ok) {
        g_variant_unref(variant);
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
    GVariant *variant = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "NameHasOwner", g_variant_new("(s)", name), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE,
        2000, nullptr, nullptr);
    gboolean owned = FALSE;
    if (variant != nullptr) {
        g_variant_get(variant, "(b)", &owned);
        g_variant_unref(variant);
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
    GVariant *seats_reply = g_dbus_connection_call_sync(
        bus, FDO_DM, FDO_DM_PATH, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", FDO_DM, "Seats"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1,
        nullptr, error);
    if (seats_reply == nullptr)
        return nullptr;
    GVariant *seats = nullptr;
    g_variant_get(seats_reply, "(v)", &seats);
    char *path = nullptr;
    if (g_variant_is_of_type(seats, G_VARIANT_TYPE_OBJECT_PATH_ARRAY) &&
        g_variant_n_children(seats) > 0)
        g_variant_get_child(seats, 0, "o", &path);
    else
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "the display manager has no seats");
    g_variant_unref(seats);
    g_variant_unref(seats_reply);
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
    GVariant *variant = nullptr;
    if (fdo) {
        char *seat = dm_seat(bus, &error);
        if (seat != nullptr)
            variant = g_dbus_connection_call_sync(bus, FDO_DM, seat, FDO_DM_SEAT, "SwitchToGreeter",
                                                  nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                                                  nullptr, &error);
        g_free(seat);
    } else {
        variant = g_dbus_connection_call_sync(bus, GDM, GDM_FACTORY_PATH, GDM_FACTORY,
                                              "CreateTransientDisplay", nullptr, nullptr,
                                              G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    }
    bool ok = variant != nullptr;
    if (ok) {
        g_variant_unref(variant);
    } else {
        fprintf(stderr, "xd-shutdown: cannot switch user: %s\n",
                error != nullptr ? error->message : "no display manager seat");
        g_clear_error(&error);
    }
    g_object_unref(bus);
    return ok;
}

static bool perform(action chosen_action, bool simulate) {
    /* Windows Logoff or Exit Windows, played out before the session ends. */
    if (chosen_action == ACT_LOGOFF || chosen_action == ACT_SHUTDOWN ||
        chosen_action == ACT_RESTART)
        xd_sound_play_event_wait(chosen_action == ACT_LOGOFF ? XD_SOUND_LOGOFF : XD_SOUND_EXIT,
                                 5000);
    if (chosen_action == ACT_LOGOFF) {
        pid_t pid = compositor_pid();
        if (pid == 0)
            return terminate_session();
        return kill(pid, SIGTERM) == 0;
    }
    if (simulate) {
        fprintf(stderr, "xd-shutdown: nested session, not running: systemctl %s\n",
                actions[chosen_action].systemctl);
        return true;
    }
    char prog[] = "systemctl";
    char *verb = xd_str_dup_cstr(xd_str_from_cstr(actions[chosen_action].systemctl));
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
    xd_strbuf contents = {0};
    *standby = false;
    *hibernate = false;
    if (xd_read_file("/sys/power/state", 4096, &contents)) {
        xd_str rest = xd_str_trim(xd_strbuf_view(&contents));
        while (rest.len > 0) {
            size_t sp = rest.len;
            bool more = xd_str_find_char(rest, ' ', &sp);
            xd_str word = xd_str_sub(rest, 0, sp);
            *standby = *standby || xd_str_eq(word, XD_STR("mem"));
            *hibernate = *hibernate || xd_str_eq(word, XD_STR("disk"));
            rest = more ? xd_str_sub(rest, sp + 1, rest.len) : XD_STR("");
        }
    }
    xd_strbuf_free(&contents);
}

static void on_change(xd_widget *combo, void *user) {
    dialog *state = user;
    int i = xd_combo_selected(combo);
    if (i >= 0 && (size_t)i < state->count)
        xd_widget_set_text(state->description, actions[state->items[i]].description);
}

static void on_ok([[maybe_unused]] xd_widget *widget, void *user) {
    dialog *state = user;
    int i = xd_combo_selected(state->combo);
    if (i < 0 || (size_t)i >= state->count)
        return;
    action chosen_action = state->items[i];
    char *key = xd_str_dup_cstr(xd_str_from_cstr(actions[chosen_action].key));
    if (!xd_state_write_lines("shutdown-choice", &key, 1))
        fprintf(stderr, "xd-shutdown: cannot save the last choice\n");
    xd_free(key);
    xd_window_close(state->win);
    perform(chosen_action, state->simulate);
}

static void on_cancel([[maybe_unused]] xd_widget *widget, void *user) {
    dialog *state = user;
    xd_window_close(state->win);
}

static void build_shutdown(dialog *state) {
    state->win = xd_window_create(state->ui, "Shut Down XeonDesktop", "xd-shutdown", 412, 208);
    xd_banner(state->win, (xd_rect){0, 0, 412, 72}, "XeonDesktop", "Professional");
    xd_image(state->win, (xd_rect){12, 86, 32, 32}, "system-shutdown");
    xd_label(state->win, (xd_rect){56, 86, 344, 16}, "What do you want the computer to do?", 0);
    state->combo = xd_combo(state->win, (xd_rect){56, 106, 220, 21}, false, on_change, state);
    state->description = xd_label(state->win, (xd_rect){56, 134, 344, 32}, "", XD_LABEL_WRAP);

    bool standby;
    bool hibernate;
    sleep_states(&standby, &hibernate);
    const char *user = getenv("USER");
    for (size_t action_index = 0; action_index < XD_ARRAY_LEN(actions); action_index++) {
        if ((action_index == ACT_STANDBY && !standby) ||
            (action_index == ACT_HIBERNATE && !hibernate))
            continue;
        xd_strbuf label = {0};
        if (action_index == ACT_LOGOFF)
            xd_strbuf_appendf(&label, "Log off %s", user != nullptr ? user : "");
        else
            xd_strbuf_append(&label, xd_str_from_cstr(actions[action_index].label));
        xd_combo_add(state->combo, xd_strbuf_cstr(&label));
        xd_strbuf_free(&label);
        state->items[state->count++] = (action)action_index;
    }

    /* Start on the last choice, like Windows; Shut down otherwise. */
    char **last = nullptr;
    size_t count = 0;
    xd_state_read_lines("shutdown-choice", &last, &count, 1);
    int pick = 0;
    for (size_t i = 0; i < state->count; i++) {
        if (count > 0 ? strcmp(actions[state->items[i]].key, last[0]) == 0
                      : state->items[i] == ACT_SHUTDOWN)
            pick = (int)i;
    }
    xd_free_lines(last, count);
    xd_combo_select(state->combo, pick);
    on_change(state->combo, state);

    if (state->simulate)
        xd_label(state->win, (xd_rect){12, 177, 140, 16}, "Test session: simulated", XD_LABEL_DIM);
    xd_widget *ok = xd_button(state->win, (xd_rect){163, 173, 75, 23}, "OK", on_ok, state);
    xd_widget *cancel =
        xd_button(state->win, (xd_rect){244, 173, 75, 23}, "Cancel", on_cancel, state);
    xd_widget *help = xd_button(state->win, (xd_rect){325, 173, 75, 23}, "Help", nullptr, nullptr);
    xd_widget_set_enabled(help, false);
    xd_window_set_default(state->win, ok);
    xd_window_set_cancel(state->win, cancel);
    xd_window_set_focus(state->win, state->combo);
}

static void on_logoff_yes([[maybe_unused]] xd_widget *widget, void *user) {
    dialog *state = user;
    xd_window_close(state->win);
    perform(ACT_LOGOFF, state->simulate);
}

/* Windows 2000's "Are you sure you want to log off?" box. */
static void build_logoff(dialog *state) {
    state->win = xd_window_create(state->ui, "Log Off XeonDesktop", "xd-shutdown", 300, 100);
    xd_image(state->win, (xd_rect){12, 14, 32, 32}, "system-log-out");
    xd_label(state->win, (xd_rect){56, 22, 232, 16}, "Are you sure you want to log off?", 0);
    xd_widget *yes = xd_button(state->win, (xd_rect){72, 62, 75, 23}, "Yes", on_logoff_yes, state);
    xd_widget *no = xd_button(state->win, (xd_rect){153, 62, 75, 23}, "No", on_cancel, state);
    xd_window_set_default(state->win, yes);
    xd_window_set_cancel(state->win, no);
    xd_window_set_focus(state->win, yes);
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

static void draw_top_band(xd_widget *widget, cairo_t *cairo, xd_rect rect, const xd_theme *theme,
                          [[maybe_unused]] void *user) {
    (void)widget;
    xd_fill_rect(cairo, rect, xd_rgba_hex(XP_DARK));
    /* The bright rule under the band, fading out at both ends. */
    int rule = XD_MAX(xd_tpx(theme, 2), 1);
    xd_rect line = {rect.x, rect.y + rect.h - rule, rect.w, rule};
    cairo_pattern_t *pattern = cairo_pattern_create_linear(rect.x, 0, rect.x + rect.w, 0);
    cairo_pattern_add_color_stop_rgb(pattern, 0, 0x5A / 255.0, 0x7E / 255.0, 0xDC / 255.0);
    cairo_pattern_add_color_stop_rgb(pattern, 0.35, 1, 1, 1);
    cairo_pattern_add_color_stop_rgb(pattern, 1, 0x5A / 255.0, 0x7E / 255.0, 0xDC / 255.0);
    cairo_rectangle(cairo, line.x, line.y, line.w, line.h);
    cairo_set_source(cairo, pattern);
    cairo_fill(cairo);
    cairo_pattern_destroy(pattern);

    xd_font_spec font = {.size_pt = 14};
    snprintf(font.family, sizeof(font.family), "%s", XD_FONT_FAMILY_FRANKLIN);
    xd_text_style st = xd_text(theme, XD_FONT_MESSAGE, xd_rgba_hex(0xFFFFFF), XD_ALIGN_START);
    st.font = &font;
    xd_draw_text(
        cairo,
        (xd_rect){rect.x + xd_tpx(theme, 11), rect.y, rect.w - xd_tpx(theme, 60), rect.h - rule},
        "Log Off XeonDesktop", &st);
    int logo = xd_tpx(theme, 30);
    cairo_new_path(cairo); /* the text left a current point */
    xd_draw_start_logo(cairo, (xd_rect){rect.x + rect.w - logo - xd_tpx(theme, 10),
                                        rect.y + (rect.h - rule - logo) / 2, logo, logo});
}

static void draw_band(xd_widget *widget, cairo_t *cairo, xd_rect rect,
                      [[maybe_unused]] const xd_theme *theme, void *user) {
    (void)widget;
    xd_fill_rect(cairo, rect, xd_rgba_hex((uint32_t)(uintptr_t)user));
}

/* Rounded tile with a white edge and a vertical gradient. */
static void tile_base(cairo_t *cairo, const xd_theme *theme, xd_rect rect, uint32_t top,
                      uint32_t bottom) {
    double rad = xd_tpx(theme, 3);
    double lw = XD_MAX(xd_tpx(theme, 1), 1);
    cairo_new_path(cairo);
    xd_path_rounded(cairo, rect.x + lw / 2, rect.y + lw / 2, rect.w - lw, rect.h - lw, rad, rad,
                    rad, rad);
    cairo_pattern_t *pattern = cairo_pattern_create_linear(0, rect.y, 0, rect.y + rect.h);
    xd_rgba top_color = xd_rgba_hex(top);
    xd_rgba bottom_color = xd_rgba_hex(bottom);
    cairo_pattern_add_color_stop_rgb(pattern, 0, top_color.r, top_color.g, top_color.b);
    cairo_pattern_add_color_stop_rgb(pattern, 1, bottom_color.r, bottom_color.g, bottom_color.b);
    cairo_set_source(cairo, pattern);
    cairo_fill_preserve(cairo);
    cairo_pattern_destroy(pattern);
    xd_set_color(cairo, xd_rgba_hex(0xFFFFFF));
    cairo_set_line_width(cairo, lw);
    cairo_stroke(cairo);
}

/* A head and shoulders. */
static void person(cairo_t *cairo, double cx, double cy, double unit) {
    cairo_new_path(cairo);
    cairo_arc(cairo, cx, cy - 3 * unit, 2.6 * unit, 0, 2 * XD_PI);
    cairo_fill(cairo);
    cairo_new_path(cairo);
    cairo_arc(cairo, cx, cy + 5.5 * unit, 5 * unit, XD_PI, 2 * XD_PI);
    cairo_close_path(cairo);
    cairo_fill(cairo);
}

/* Switch User: a green tile with two people; Log Off: the Start menu's
 * orange tile with a padlock. Greyed out, the tile loses its colour. */
static void draw_tile_icon(cairo_t *cairo, const xd_theme *theme, xd_rect rect, tile which,
                           bool enabled) {
    double cx = rect.x + rect.w / 2.0, cy = rect.y + rect.h / 2.0, unit = rect.w / 22.0;
    if (which == TILE_SWITCH) {
        tile_base(cairo, theme, rect, enabled ? 0x8CD36A : 0xC8C8C8, enabled ? 0x2E8B1E : 0x8C8C8C);
        xd_set_color(cairo, xd_rgba_hex(enabled ? 0xBFE8A8 : 0xE0E0E0));
        person(cairo, cx + 3 * unit, cy - unit, unit * 0.9);
        xd_set_color(cairo, xd_rgba_hex(0xFFFFFF));
        person(cairo, cx - 2.5 * unit, cy + 0.5 * unit, unit);
        return;
    }
    uint32_t bottom = enabled ? 0xD9780A : 0x8C8C8C;
    tile_base(cairo, theme, rect, enabled ? 0xF9C15A : 0xC8C8C8, bottom);
    xd_set_color(cairo, xd_rgba_hex(0xFFFFFF));
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cairo, 2 * unit);
    cairo_new_path(cairo);
    cairo_arc(cairo, cx, cy - 2 * unit, 4 * unit, XD_PI, 2 * XD_PI);
    cairo_stroke(cairo);
    cairo_rectangle(cairo, cx - 6 * unit, cy - unit, 12 * unit, 9 * unit);
    cairo_fill(cairo);
    xd_set_color(cairo, xd_rgba_hex(bottom));
    cairo_rectangle(cairo, cx - unit, cy + 2 * unit, 2 * unit, 3 * unit);
    cairo_fill(cairo);
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_BUTT);
}

static tile tile_of(const dialog *state, const xd_widget *widget) {
    return widget == state->tiles[TILE_SWITCH] ? TILE_SWITCH : TILE_LOGOFF;
}

static void draw_tile(xd_widget *widget, cairo_t *cairo, xd_rect rect, const xd_theme *theme,
                      void *user) {
    dialog *state = user;
    tile which = tile_of(state, widget);
    bool enabled = state->tile_enabled[which];
    xd_fill_rect(cairo, rect, xd_rgba_hex(XP_LIGHT));
    int icon = xd_tpx(theme, TILE_ICON);
    int down = state->pressed == (int)which ? xd_tpx(theme, 1) : 0;
    xd_rect ir = {rect.x + (rect.w - icon) / 2 + down, rect.y + xd_tpx(theme, 4) + down, icon,
                  icon};
    draw_tile_icon(cairo, theme, ir, which, enabled);
    xd_text_style st = xd_text(theme, XD_FONT_MESSAGE, xd_rgba_hex(enabled ? 0xFFFFFF : 0xA9BCEC),
                               XD_ALIGN_CENTER);
    xd_draw_text(cairo,
                 (xd_rect){rect.x, ir.y + icon + xd_tpx(theme, 6), rect.w, xd_tpx(theme, 16)},
                 tile_label[which], &st);
}

static void tile_pointer(xd_widget *widget, xd_pointer_event ev, double x, double y, void *user) {
    dialog *state = user;
    tile which = tile_of(state, widget);
    if (!state->tile_enabled[which])
        return;
    bool inside = x >= 0 && y >= 0 && x < (double)TILE_W && y < (double)TILE_H;
    int was = state->pressed;
    if (ev == XD_POINTER_PRESS || ev == XD_POINTER_MOTION)
        state->pressed = inside ? (int)which : -1;
    else
        state->pressed = -1;
    if (state->pressed != was)
        xd_widget_redraw(widget);
    if (ev != XD_POINTER_RELEASE || !inside)
        return;
    xd_window_close(state->win);
    if (which == TILE_SWITCH)
        switch_user();
    else
        perform(ACT_LOGOFF, state->simulate);
}

/* XP's "Log Off Windows" with Fast User Switching on: Switch User keeps
 * this session running and goes to the Welcome screen (the display
 * manager's greeter), Log Off ends it. */
static void build_logoff_xp(dialog *state) {
    state->win = xd_window_create(state->ui, "Log Off XeonDesktop", "xd-shutdown", XP_W, XP_H);
    state->pressed = -1;
    xd_custom(state->win, (xd_rect){0, 0, XP_W, XP_TOP_H}, draw_top_band, state);
    xd_custom(state->win, (xd_rect){0, XP_TOP_H, XP_W, XP_BOTTOM_Y - XP_TOP_H}, draw_band,
              (void *)(uintptr_t)XP_LIGHT);
    xd_custom(state->win, (xd_rect){0, XP_BOTTOM_Y, XP_W, XP_H - XP_BOTTOM_Y}, draw_band,
              (void *)(uintptr_t)XP_DARK);
    state->tile_enabled[TILE_SWITCH] = can_switch_user();
    state->tile_enabled[TILE_LOGOFF] = true;
    for (int i = 0; i < TILE_COUNT; i++) {
        state->tiles[i] =
            xd_custom(state->win, (xd_rect){tile_x[i], TILE_Y, TILE_W, TILE_H}, draw_tile, state);
        xd_custom_set_pointer(state->tiles[i], tile_pointer);
    }
    xd_widget *cancel = xd_button(state->win, (xd_rect){XP_W - 75 - 12, XP_BOTTOM_Y + 13, 75, 23},
                                  "Cancel", on_cancel, state);
    xd_window_set_default(state->win, cancel);
    xd_window_set_cancel(state->win, cancel);
    xd_window_set_focus(state->win, cancel);
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

    dialog state = {0};
    const char *nested = getenv("XEONDESKTOP_NESTED");
    state.simulate = dry_run || nested == nullptr || strcmp(nested, "0") != 0;

    /* Windows 7's Shut Down button acts at once; the same safety rules
     * (nested sessions only simulate) apply. */
    if (direct != nullptr) {
        /* The Task Manager's Disconnect on our own session. */
        if (strcmp(direct, "switchuser") == 0)
            return switch_user() ? EXIT_SUCCESS : EXIT_FAILURE;
        for (size_t action_index = 0; action_index < XD_ARRAY_LEN(actions); action_index++) {
            if (strcmp(actions[action_index].key, direct) == 0)
                return perform((action)action_index, state.simulate) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        fprintf(stderr, "xd-shutdown: unknown action %s\n", direct);
        return EXIT_FAILURE;
    }
    state.ui = xd_ui_connect(argc, argv);
    if (state.ui == nullptr)
        return EXIT_FAILURE;
    if (logoff && xd_ui_style(state.ui) == XD_STYLE_LUNA)
        build_logoff_xp(&state);
    else if (logoff)
        build_logoff(&state);
    else
        build_shutdown(&state);
    xd_window_show(state.win);
    xd_ui_run(state.ui);
    xd_ui_destroy(state.ui);
    return EXIT_SUCCESS;
}
