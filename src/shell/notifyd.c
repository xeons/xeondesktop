/* SPDX-License-Identifier: MIT
 * Programs' notifications (org.freedesktop.Notifications, the
 * Desktop Notifications Specification 1.2), shown as XP's balloons from
 * the notification area, one at a time, as Windows queued them: one
 * waits while another balloon shows, or while a menu is open.
 *
 * A balloon that goes by itself leaves its notification in the history
 * (notifyfly.c), unread; so does one that comes with Do Not Disturb on, or
 * while a full-screen window is in front (not an urgent one). Its program
 * hears of it as still open ("persistence"), until it's dismissed there or
 * its program closes it. The history outlives the session, without what
 * only the program could act on (actions, pictures).
 *
 * Taken from another server (mako, dunst) only in a real session: a test
 * or nested session leaves the desktop it runs in its own.
 */
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <string.h>

#include "../settings/soundplay.h"
#include "../settings/state.h"
#include "apps.h"
#include "notices.h"
#include "shell.h"

#ifndef XD_VERSION
#define XD_VERSION "0"
#endif

#define NOTIFY_NAME "org.freedesktop.Notifications"
#define NOTIFY_PATH "/org/freedesktop/Notifications"

static constexpr char ITEM[] = "notifications";
static constexpr char STATE_FILE[] = "notifications.ini";
/* The most kept, shown or not. */
static constexpr size_t KEEP_MAX = 50;
/* Waiting for a balloon: more than this go to the history unseen. */
static constexpr guint QUEUE_MAX = 3;
/* A balloon's time, as XP's (at least ten seconds); a low-urgency one's;
 * the bounds of a time the program chose. */
static constexpr unsigned SHOW_SECONDS = 10;
static constexpr unsigned LOW_SECONDS = 6;
static constexpr unsigned MIN_SECONDS = 3;
static constexpr unsigned MAX_SECONDS = 60;
/* Another balloon or a menu in the way: looking again after this. */
static constexpr guint RETRY_MS = 1000;
static constexpr guint SAVE_DELAY_SECONDS = 2;

/* NotificationClosed's reasons. */
enum {
    CLOSED_EXPIRED = 1,
    CLOSED_DISMISSED = 2,
    CLOSED_BY_CALL = 3,
    CLOSED_UNDEFINED = 4,
};

static const char notify_xml[] =
    "<node>"
    "  <interface name='" NOTIFY_NAME "'>"
    "    <method name='GetCapabilities'>"
    "      <arg name='capabilities' type='as' direction='out'/>"
    "    </method>"
    "    <method name='Notify'>"
    "      <arg name='app_name' type='s' direction='in'/>"
    "      <arg name='replaces_id' type='u' direction='in'/>"
    "      <arg name='app_icon' type='s' direction='in'/>"
    "      <arg name='summary' type='s' direction='in'/>"
    "      <arg name='body' type='s' direction='in'/>"
    "      <arg name='actions' type='as' direction='in'/>"
    "      <arg name='hints' type='a{sv}' direction='in'/>"
    "      <arg name='expire_timeout' type='i' direction='in'/>"
    "      <arg name='id' type='u' direction='out'/>"
    "    </method>"
    "    <method name='CloseNotification'>"
    "      <arg name='id' type='u' direction='in'/>"
    "    </method>"
    "    <method name='GetServerInformation'>"
    "      <arg name='name' type='s' direction='out'/>"
    "      <arg name='vendor' type='s' direction='out'/>"
    "      <arg name='version' type='s' direction='out'/>"
    "      <arg name='spec_version' type='s' direction='out'/>"
    "    </method>"
    "    <signal name='NotificationClosed'>"
    "      <arg name='id' type='u'/>"
    "      <arg name='reason' type='u'/>"
    "    </signal>"
    "    <signal name='ActionInvoked'>"
    "      <arg name='id' type='u'/>"
    "      <arg name='action_key' type='s'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

struct xd_notifyd {
    xd_shell *shell;
    xd_notices store;
    GDBusNodeInfo *info;
    guint own_id;
    GDBusConnection *bus;
    guint object_id;
    guint owner_changed; /* signal subscription */
    GQueue queue;        /* ids waiting for a balloon, oldest first */
    uint32_t showing;    /* its balloon's id; 0: none */
    uint32_t replacing;  /* its balloon is being replaced by its new content */
    guint pump_source;
    guint save_source;
};

/* ---- telling programs ------------------------------------------------------------------ */

static void emit(xd_notifyd *notifyd, const xd_notice *notice, const char *signal,
                 GVariant *parameters) {
    /* To its own program only: no other needs to know. */
    if (notifyd->bus == nullptr || notice->sender == nullptr || notice->closed) {
        g_variant_unref(g_variant_ref_sink(parameters));
        return;
    }
    g_dbus_connection_emit_signal(notifyd->bus, notice->sender, NOTIFY_PATH, NOTIFY_NAME, signal,
                                  parameters, nullptr);
}

static void tell_closed(xd_notifyd *notifyd, xd_notice *notice, uint32_t reason) {
    emit(notifyd, notice, "NotificationClosed", g_variant_new("(uu)", notice->id, reason));
    notice->closed = true;
}

/* ---- keeping ------------------------------------------------------------------------- */

static gboolean save_now(gpointer data) {
    xd_notifyd *notifyd = data;
    notifyd->save_source = 0;
    xd_strbuf path = {0};
    xd_state_path(STATE_FILE, &path);
    char *contents = xd_notices_save_data(&notifyd->store);
    GError *error = nullptr;
    if (!g_file_set_contents_full(path.data, contents, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                  &error)) {
        g_printerr("xd-shell: cannot keep the notifications: %s\n", error->message);
        g_error_free(error);
    }
    g_free(contents);
    xd_strbuf_free(&path);
    return G_SOURCE_REMOVE;
}

/* Something changed: the icon, the history, and later the file. */
static void changed(xd_notifyd *notifyd) {
    xd_tray_changed(notifyd->shell);
    xd_notify_flyout_changed(notifyd->shell);
    if (notifyd->save_source == 0)
        notifyd->save_source = g_timeout_add_seconds(SAVE_DELAY_SECONDS, save_now, notifyd);
}

static void unqueue(xd_notifyd *notifyd, uint32_t id) {
    g_queue_remove_all(&notifyd->queue, GUINT_TO_POINTER(id));
}

/* Forgets it, its balloon put away. */
static void forget(xd_notifyd *notifyd, uint32_t id) {
    unqueue(notifyd, id);
    xd_notices_remove(&notifyd->store, id);
    if (notifyd->showing == id) {
        notifyd->showing = 0;
        xd_balloon_hide_tray(notifyd->shell, ITEM);
    }
}

/* ---- balloons ------------------------------------------------------------------------- */

/* Quiet: Do Not Disturb, or a full-screen window in front (a game, a
 * film, a presentation). */
static bool quiet(const xd_notifyd *notifyd) {
    xd_shell *shell = notifyd->shell;
    if (shell->settings.do_not_disturb)
        return true;
    XD_LIST_FOR_EACH(toplevel, &shell->toplevels, xd_toplevel, link) {
        if (toplevel->activated && toplevel->fullscreen && !toplevel->minimized)
            return true;
    }
    return false;
}

/* Its program's icon, by its desktop file. g_free. */
static char *program_icon(const xd_notice *notice) {
    GDesktopAppInfo *info = notice->desktop_entry != nullptr
                                ? g_desktop_app_info_new(notice->desktop_entry)
                                : nullptr;
    GIcon *icon = info != nullptr ? g_app_info_get_icon(G_APP_INFO(info)) : nullptr;
    char *name = nullptr;
    if (icon != nullptr && G_IS_THEMED_ICON(icon)) {
        const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(icon));
        name = names != nullptr && names[0] != nullptr ? g_strdup(names[0]) : nullptr;
    }
    g_clear_object(&info);
    return name;
}

static unsigned seconds_for(const xd_notice *notice) {
    if (notice->urgency == XD_NOTICE_CRITICAL || notice->expire_ms == 0)
        return 0; /* until it's closed */
    if (notice->expire_ms > 0)
        return XD_CLAMP(((unsigned)notice->expire_ms + 999) / 1000, MIN_SECONDS, MAX_SECONDS);
    return notice->urgency == XD_NOTICE_LOW ? LOW_SECONDS : SHOW_SECONDS;
}

static void schedule_pump(xd_notifyd *notifyd, guint delay_ms);

/* Its action `key` taken (a click on its balloon or in the history):
 * its program is told, or without one listening (or a "default" it
 * doesn't have), its program is opened. Then it's gone, unless it stays. */
static void take_action(xd_notifyd *notifyd, xd_notice *notice, const char *key) {
    bool live = notice->sender != nullptr && !notice->closed;
    if (live && key != nullptr && xd_notice_action_label(notice, key) != nullptr) {
        emit(notifyd, notice, "ActionInvoked", g_variant_new("(us)", notice->id, key));
    } else if (notice->desktop_entry != nullptr) {
        GDesktopAppInfo *info = g_desktop_app_info_new(notice->desktop_entry);
        if (info != nullptr)
            xd_app_launch_info(info, nullptr);
        g_clear_object(&info);
    }
    if (notice->resident && live)
        return;
    uint32_t id = notice->id;
    if (live)
        tell_closed(notifyd, notice, CLOSED_DISMISSED);
    forget(notifyd, id);
}

static void balloon_ended(xd_shell *shell, xd_balloon_end end, size_t button, void *data) {
    xd_notifyd *notifyd = shell->notifyd;
    uint32_t id = GPOINTER_TO_UINT(data);
    if (notifyd == nullptr)
        return;
    if (end == XD_BALLOON_HIDDEN && notifyd->replacing == id)
        return; /* its new content is taking its place */
    if (notifyd->showing == id)
        notifyd->showing = 0;
    xd_notice *notice = xd_notices_find(&notifyd->store, id);
    if (notice != nullptr) {
        switch (end) {
        case XD_BALLOON_TIMED_OUT:
        case XD_BALLOON_HIDDEN:
            if (notice->transient || (notice->expire_ms > 0 && end == XD_BALLOON_TIMED_OUT)) {
                /* Its time is up, as its program asked. */
                tell_closed(notifyd, notice, CLOSED_EXPIRED);
                if (notice->transient)
                    forget(notifyd, id);
                else
                    notice->unread = true;
            } else {
                notice->unread = true;
            }
            break;
        case XD_BALLOON_CLOSED:
            tell_closed(notifyd, notice, CLOSED_DISMISSED);
            forget(notifyd, id);
            break;
        case XD_BALLOON_CLICKED:
            take_action(notifyd, notice, "default");
            break;
        case XD_BALLOON_ACTION:
            take_action(notifyd, notice, xd_notice_button_key(notice, button));
            break;
        }
    }
    changed(notifyd);
    schedule_pump(notifyd, 0);
}

/* `sound`: not for new content in a balloon already showing (a
 * download's progress). */
static void show(xd_notifyd *notifyd, xd_notice *notice, bool sound) {
    xd_shell *shell = notifyd->shell;
    const char *labels[8];
    size_t count = XD_MIN(xd_notice_button_count(notice), XD_ARRAY_LEN(labels));
    for (size_t i = 0; i < count; i++)
        labels[i] = xd_notice_action_label(notice, xd_notice_button_key(notice, i));
    char *fallback = notice->icon == nullptr ? program_icon(notice) : nullptr;
    xd_balloon_notice balloon = {
        .item = ITEM,
        .icon = notice->icon != nullptr ? notice->icon
                : fallback != nullptr   ? fallback
                                        : "dialog-information",
        .image = notice->image,
        .title = notice->summary[0] != '\0' ? notice->summary : notice->app_name,
        .text = notice->body,
        .buttons = labels,
        .button_count = count,
        .seconds = seconds_for(notice),
        .ended = balloon_ended,
        .data = GUINT_TO_POINTER(notice->id),
    };
    notifyd->showing = notice->id;
    notice->unread = false;
    xd_balloon_notify(shell, &balloon);
    g_free(fallback);
    if (sound && !notice->suppress_sound)
        xd_sound_play_event(XD_SOUND_NOTIFICATION);
}

/* The next waiting one, as a balloon, when there's room for it. */
static void pump(xd_notifyd *notifyd) {
    xd_shell *shell = notifyd->shell;
    if (notifyd->showing != 0)
        return;
    while (!g_queue_is_empty(&notifyd->queue)) {
        uint32_t id = GPOINTER_TO_UINT(g_queue_peek_head(&notifyd->queue));
        xd_notice *notice = xd_notices_find(&notifyd->store, id);
        if (notice == nullptr || notice->closed) {
            g_queue_pop_head(&notifyd->queue);
            continue;
        }
        if (quiet(notifyd) && notice->urgency != XD_NOTICE_CRITICAL) {
            /* Straight to the history. */
            g_queue_pop_head(&notifyd->queue);
            notice->unread = true;
            changed(notifyd);
            continue;
        }
        if (xd_balloon_busy(shell) || shell->start_menu != nullptr || shell->start7 != nullptr) {
            schedule_pump(notifyd, RETRY_MS);
            return;
        }
        g_queue_pop_head(&notifyd->queue);
        show(notifyd, notice, true);
        return;
    }
}

static gboolean pump_now(gpointer data) {
    xd_notifyd *notifyd = data;
    notifyd->pump_source = 0;
    pump(notifyd);
    return G_SOURCE_REMOVE;
}

static void schedule_pump(xd_notifyd *notifyd, guint delay_ms) {
    if (notifyd->pump_source != 0) {
        if (delay_ms > 0)
            return; /* one's coming already */
        g_source_remove(notifyd->pump_source);
    }
    notifyd->pump_source = delay_ms > 0 ? g_timeout_add(delay_ms, pump_now, notifyd)
                                        : g_idle_add(pump_now, notifyd);
}

void xd_notifyd_balloon_gone(xd_shell *shell) {
    if (shell->notifyd != nullptr && !g_queue_is_empty(&shell->notifyd->queue))
        schedule_pump(shell->notifyd, 0);
}

/* ---- the bus ---------------------------------------------------------------------------- */

static void enqueue(xd_notifyd *notifyd, xd_notice *notice) {
    unqueue(notifyd, notice->id);
    g_queue_push_tail(&notifyd->queue, GUINT_TO_POINTER(notice->id));
    /* A burst: the oldest waiting go to the history unseen. */
    while (g_queue_get_length(&notifyd->queue) > QUEUE_MAX) {
        uint32_t id = GPOINTER_TO_UINT(g_queue_pop_head(&notifyd->queue));
        xd_notice *older = xd_notices_find(&notifyd->store, id);
        if (older != nullptr)
            older->unread = true;
    }
}

static void notify(xd_notifyd *notifyd, GVariant *parameters, const char *sender,
                   GDBusMethodInvocation *invocation) {
    uint32_t replaces_id = 0;
    GError *error = nullptr;
    xd_notice *notice = xd_notice_parse(parameters, sender, &replaces_id, &error);
    if (notice == nullptr) {
        g_dbus_method_invocation_return_gerror(invocation, error);
        g_error_free(error);
        return;
    }
    bool replaced = false;
    notice = xd_notices_add(&notifyd->store, notice, replaces_id, &replaced);
    uint32_t id = notice->id;
    if (replaced && notifyd->showing == id) {
        /* Its balloon shows the new content. */
        notifyd->replacing = id;
        show(notifyd, notice, false);
        notifyd->replacing = 0;
    } else if (!replaced || g_queue_find(&notifyd->queue, GUINT_TO_POINTER(id)) == nullptr) {
        enqueue(notifyd, notice);
    }
    GArray *gone = xd_notices_trim(&notifyd->store, KEEP_MAX, notifyd->showing);
    for (guint i = 0; i < gone->len; i++) {
        uint32_t old = g_array_index(gone, uint32_t, i);
        unqueue(notifyd, old);
        if (notifyd->bus != nullptr)
            g_dbus_connection_emit_signal(notifyd->bus, nullptr, NOTIFY_PATH, NOTIFY_NAME,
                                          "NotificationClosed",
                                          g_variant_new("(uu)", old, CLOSED_UNDEFINED), nullptr);
    }
    g_array_unref(gone);
    changed(notifyd);
    schedule_pump(notifyd, 0);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", id));
}

static void close_notification(xd_notifyd *notifyd, uint32_t id, const char *sender) {
    xd_notice *notice = xd_notices_find(&notifyd->store, id);
    /* Only its own program closes it. */
    if (notice == nullptr || notice->closed || g_strcmp0(notice->sender, sender) != 0)
        return;
    tell_closed(notifyd, notice, CLOSED_BY_CALL);
    forget(notifyd, id);
    changed(notifyd);
}

static void handle_method([[maybe_unused]] GDBusConnection *connection, const char *sender,
                          [[maybe_unused]] const char *path, [[maybe_unused]] const char *iface,
                          const char *method, GVariant *parameters,
                          GDBusMethodInvocation *invocation, gpointer data) {
    xd_notifyd *notifyd = data;
    if (strcmp(method, "Notify") == 0) {
        notify(notifyd, parameters, sender, invocation);
    } else if (strcmp(method, "CloseNotification") == 0) {
        uint32_t id = 0;
        g_variant_get(parameters, "(u)", &id);
        close_notification(notifyd, id, sender);
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (strcmp(method, "GetCapabilities") == 0) {
        /* Markup is taken out, not shown; sounds are the scheme's
         * Notification sound, or none (suppress-sound). */
        static const char *const capabilities[] = {"actions",     "body",        "body-markup",
                                                   "icon-static", "persistence", "sound"};
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
        for (size_t i = 0; i < XD_ARRAY_LEN(capabilities); i++)
            g_variant_builder_add(&builder, "s", capabilities[i]);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(as)", &builder));
    } else if (strcmp(method, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(ssss)", "XeonDesktop", "XeonDesktop", XD_VERSION, "1.2"));
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_METHOD, "No method %s", method);
    }
}

static const GDBusInterfaceVTable vtable = {.method_call = handle_method};

/* A program gone from the bus (notify-send goes at once): its
 * notifications still show, and stay in the history, with no one to tell
 * of their actions (a click opens the program instead). */
static void handle_owner_changed([[maybe_unused]] GDBusConnection *connection,
                                 [[maybe_unused]] const char *sender,
                                 [[maybe_unused]] const char *path,
                                 [[maybe_unused]] const char *iface,
                                 [[maybe_unused]] const char *signal, GVariant *parameters,
                                 gpointer data) {
    xd_notifyd *notifyd = data;
    const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
    g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
    if (name[0] != ':' || new_owner[0] != '\0')
        return;
    for (guint i = 0; i < notifyd->store.list->len; i++) {
        xd_notice *notice = g_ptr_array_index(notifyd->store.list, i);
        if (g_strcmp0(notice->sender, name) == 0)
            g_clear_pointer(&notice->sender, g_free);
    }
}

static void bus_acquired(GDBusConnection *connection, [[maybe_unused]] const char *name,
                         gpointer data) {
    xd_notifyd *notifyd = data;
    notifyd->bus = g_object_ref(connection);
    GError *error = nullptr;
    notifyd->object_id = g_dbus_connection_register_object(
        connection, NOTIFY_PATH, notifyd->info->interfaces[0], &vtable, notifyd, nullptr, &error);
    if (notifyd->object_id == 0) {
        g_printerr("xd-shell: notifications: %s\n", error->message);
        g_error_free(error);
    }
    notifyd->owner_changed = g_dbus_connection_signal_subscribe(
        connection, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", nullptr, G_DBUS_SIGNAL_FLAGS_NONE, handle_owner_changed, notifyd,
        nullptr);
}

static void name_lost([[maybe_unused]] GDBusConnection *connection, const char *name,
                      [[maybe_unused]] gpointer data) {
    if (connection != nullptr)
        g_printerr("xd-shell: another program shows notifications (%s is taken)\n", name);
}

/* ---- the notification area icon ---------------------------------------------------------- */

static const char *icon(void *data) {
    xd_notifyd *notifyd = data;
    xd_shell *shell = notifyd->shell;
    /* Icon themes name it differently (Breeze's, then the older names). */
    static const char *const unread[] = {"notification-active", "notifications",
                                         "preferences-system-notifications", "dialog-information"};
    static const char *const muted[] = {"notifications-disabled", "notifications",
                                        "preferences-system-notifications", "dialog-information"};
    static const char *const plain[] = {"notifications", "preferences-system-notifications",
                                        "dialog-information"};
    const char *const *names = shell->settings.do_not_disturb            ? muted
                               : xd_notices_unread(&notifyd->store) > 0 ? unread
                                                                         : plain;
    size_t count = names == plain ? XD_ARRAY_LEN(plain) : XD_ARRAY_LEN(unread);
    for (size_t i = 0; i + 1 < count; i++) {
        if (xd_icons_get(shell->icons, names[i], 16) != nullptr)
            return names[i];
    }
    return names[count - 1];
}

static char *tooltip(void *data) {
    xd_notifyd *notifyd = data;
    size_t unread = xd_notices_unread(&notifyd->store);
    const char *quiet_note = notifyd->shell->settings.do_not_disturb ? "\nDo Not Disturb is on" : "";
    if (unread == 0)
        return g_strdup_printf("No new notifications%s", quiet_note);
    return g_strdup_printf(unread == 1 ? "%zu new notification%s" : "%zu new notifications%s",
                           unread, quiet_note);
}

static void activate(void *data, xd_taskbar *bar, xd_rect anchor, uint32_t serial) {
    xd_notifyd *notifyd = data;
    xd_balloon_hide_tray(notifyd->shell, ITEM);
    xd_notify_flyout_toggle(bar, anchor, serial);
}

static void toggle_quiet(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_notifyd_set_do_not_disturb(shell, !shell->settings.do_not_disturb);
}

static void clear_all(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_notifyd_dismiss_all(shell);
}

static void menu(void *data, xd_menu_tree *tree, xd_menu_model *menu_model) {
    xd_notifyd *notifyd = data;
    xd_menu_item *it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, "&Do Not Disturb", nullptr);
    it->action = toggle_quiet;
    it->checked = notifyd->shell->settings.do_not_disturb;
    it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, "&Clear All Notifications", nullptr);
    it->action = clear_all;
    it->disabled = notifyd->store.list->len == 0;
}

static void destroy([[maybe_unused]] void *data) {
    /* Ours to free: xd_notifyd_finish. */
}

static const xd_sysicon_ops ops = {
    .icon = icon,
    .tooltip = tooltip,
    .menu = menu,
    .activate = activate,
    .destroy = destroy,
};

/* ---- the history ----------------------------------------------------------------------- */

size_t xd_notifyd_count(const xd_shell *shell) {
    return shell->notifyd != nullptr ? shell->notifyd->store.list->len : 0;
}

const xd_notice *xd_notifyd_get(const xd_shell *shell, size_t index) {
    const GPtrArray *list = shell->notifyd->store.list;
    return index < list->len ? g_ptr_array_index(list, list->len - 1 - index) : nullptr;
}

void xd_notifyd_activate(xd_shell *shell, uint32_t id) {
    xd_notifyd *notifyd = shell->notifyd;
    xd_notice *notice = notifyd != nullptr ? xd_notices_find(&notifyd->store, id) : nullptr;
    if (notice == nullptr)
        return;
    take_action(notifyd, notice, "default");
    changed(notifyd);
}

void xd_notifyd_dismiss(xd_shell *shell, uint32_t id) {
    xd_notifyd *notifyd = shell->notifyd;
    xd_notice *notice = notifyd != nullptr ? xd_notices_find(&notifyd->store, id) : nullptr;
    if (notice == nullptr)
        return;
    tell_closed(notifyd, notice, CLOSED_DISMISSED);
    forget(notifyd, id);
    changed(notifyd);
}

void xd_notifyd_dismiss_all(xd_shell *shell) {
    xd_notifyd *notifyd = shell->notifyd;
    if (notifyd == nullptr)
        return;
    while (notifyd->store.list->len > 0) {
        xd_notice *notice = g_ptr_array_index(notifyd->store.list, 0);
        tell_closed(notifyd, notice, CLOSED_DISMISSED);
        forget(notifyd, notice->id);
    }
    changed(notifyd);
}

void xd_notifyd_mark_read(xd_shell *shell) {
    xd_notifyd *notifyd = shell->notifyd;
    if (notifyd == nullptr || xd_notices_unread(&notifyd->store) == 0)
        return;
    for (guint i = 0; i < notifyd->store.list->len; i++)
        ((xd_notice *)g_ptr_array_index(notifyd->store.list, i))->unread = false;
    changed(notifyd);
}

void xd_notifyd_set_do_not_disturb(xd_shell *shell, bool on) {
    shell->settings.do_not_disturb = on;
    if (!xd_settings_save(&shell->settings))
        g_printerr("xd-shell: cannot save Do Not Disturb\n");
    if (shell->notifyd != nullptr)
        changed(shell->notifyd);
}

/* ---- start and end --------------------------------------------------------------------- */

void xd_notifyd_init(xd_shell *shell) {
    xd_notifyd *notifyd = g_new0(xd_notifyd, 1);
    notifyd->shell = shell;
    xd_notices_init(&notifyd->store);
    g_queue_init(&notifyd->queue);
    xd_strbuf path = {0};
    xd_state_path(STATE_FILE, &path);
    char *contents = nullptr;
    if (g_file_get_contents(path.data, &contents, nullptr, nullptr))
        xd_notices_load_data(&notifyd->store, contents);
    g_free(contents);
    xd_strbuf_free(&path);
    GArray *gone = xd_notices_trim(&notifyd->store, KEEP_MAX, 0);
    g_array_unref(gone);
    notifyd->info = g_dbus_node_info_new_for_xml(notify_xml, nullptr);
    shell->notifyd = notifyd;
    xd_tray_add_sysicon(shell, ITEM, &ops, notifyd);
    /* A real session's desktop shows them, whoever did before; a test's
     * or a nested one's only if no one does. */
    const char *nested = getenv("XEONDESKTOP_NESTED");
    bool real = nested != nullptr && strcmp(nested, "0") == 0;
    GBusNameOwnerFlags flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
    if (real)
        flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;
    notifyd->own_id = g_bus_own_name(G_BUS_TYPE_SESSION, NOTIFY_NAME, flags, bus_acquired,
                                     nullptr, name_lost, notifyd, nullptr);
}

void xd_notifyd_finish(xd_shell *shell) {
    xd_notifyd *notifyd = shell->notifyd;
    if (notifyd == nullptr)
        return;
    if (notifyd->save_source != 0) {
        g_source_remove(notifyd->save_source);
        save_now(notifyd);
    }
    if (notifyd->pump_source != 0)
        g_source_remove(notifyd->pump_source);
    if (notifyd->own_id != 0)
        g_bus_unown_name(notifyd->own_id);
    if (notifyd->bus != nullptr) {
        if (notifyd->object_id != 0)
            g_dbus_connection_unregister_object(notifyd->bus, notifyd->object_id);
        if (notifyd->owner_changed != 0)
            g_dbus_connection_signal_unsubscribe(notifyd->bus, notifyd->owner_changed);
        g_object_unref(notifyd->bus);
    }
    if (notifyd->info != nullptr)
        g_dbus_node_info_unref(notifyd->info);
    g_queue_clear(&notifyd->queue);
    xd_notices_finish(&notifyd->store);
    shell->notifyd = nullptr;
    g_free(notifyd);
}
