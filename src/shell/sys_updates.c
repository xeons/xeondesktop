/* SPDX-License-Identifier: MIT
 * Automatic Updates in the notification area, as XP's wuauclt did: on the
 * schedule in desktop.ini [Updates] (every day or one day a week at an
 * hour; a check missed while the computer was off runs soon after logon)
 * it runs `xd-wuaucpl --check`, which refreshes, counts and, in Download
 * mode, downloads, but never installs. What the check found is a state
 * file we watch: while updates wait there's an icon, and a new set of them
 * brings XP's balloon. A click on either opens the updates to install.
 */
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

#include "../wuaucpl/state.h"
#include "shell.h"

static constexpr char ITEM[] = "updates";
/* After logon, the first check waits this long (XP waited too, so a
 * logon isn't slowed down); XD_UPDATES_DELAY (seconds) shortens it. */
static constexpr int64_t FIRST_DELAY = 5 * 60;
/* How often the schedule is looked at: wall clock time, so suspend and
 * clock changes need no special care. */
static constexpr unsigned TICK_SECONDS = 60;
/* A check that never wrote its result is given up after this long. */
static constexpr int64_t GIVE_UP = 60 * 60;

typedef struct updates {
    xd_shell *shell;
    char *path;
    GFileMonitor *monitor;
    xd_updates_state st;
    bool loaded;
    char shown[41];      /* the set the balloon last told about */
    int64_t started;     /* when we started a check (monotonic seconds); 0: none running */
    unsigned tick;
} updates;

static int waiting(const updates *u) { return u->st.software + u->st.firmware; }

static bool turned_off(const updates *u) {
    return u->shell->settings.updates.mode == XD_UPDATES_OFF;
}

static void open_page(xd_shell *shell, const char *page) {
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-wuaucpl", page));
}

static void review(xd_shell *shell, [[maybe_unused]] void *data) {
    open_page(shell, "--page 1");
}

static void settings_page(xd_shell *shell, [[maybe_unused]] void *data) {
    open_page(shell, "--page 0");
}

static void balloon(updates *u) {
    /* XP's words: "ready" once downloaded. */
    bool ready = u->st.downloaded || u->st.software == 0;
    xd_balloon_tray(u->shell, ITEM,
                    u->st.important > 0 ? "software-update-urgent" : "software-update-available",
                    ready ? "Updates are ready for your computer"
                          : "Updates are available for your computer",
                    ready ? "Click here to install these updates."
                          : "Click here to review and install these updates.",
                    review, nullptr);
}

static void load(updates *u, bool startup) {
    xd_updates_state old = u->st;
    if (!xd_updates_state_read(u->path, &u->st)) {
        u->st = (xd_updates_state){0};
        xd_tray_changed(u->shell);
        return;
    }
    u->loaded = true;
    if (u->st.checked != old.checked)
        u->started = 0; /* the check we started (or another) has ended */
    xd_tray_changed(u->shell);
    if (waiting(u) == 0 || turned_off(u)) {
        xd_balloon_hide_tray(u->shell, ITEM);
        u->shown[0] = '\0';
        return;
    }
    /* At logon while updates wait, and when a check finds a new set; not
     * when the applet merely recounts them. */
    bool fresh = startup || (u->st.checked > old.checked &&
                             strcmp(u->st.signature, u->shown) != 0);
    if (fresh) {
        memcpy(u->shown, u->st.signature, sizeof(u->shown));
        balloon(u);
    }
}

static void on_changed([[maybe_unused]] GFileMonitor *m, [[maybe_unused]] GFile *file,
                       [[maybe_unused]] GFile *other, GFileMonitorEvent event, gpointer data) {
    if (event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || event == G_FILE_MONITOR_EVENT_CREATED ||
        event == G_FILE_MONITOR_EVENT_MOVED_IN || event == G_FILE_MONITOR_EVENT_RENAMED ||
        event == G_FILE_MONITOR_EVENT_DELETED)
        load(data, false);
}

/* Checks only in a real session, or a test's with a system bus of its
 * own: a nested session on the developer's desktop leaves the real
 * package manager alone. */
static bool may_check(void) {
    const char *nested = getenv("XEONDESKTOP_NESTED");
    return (nested != nullptr && strcmp(nested, "0") == 0) ||
           getenv("DBUS_SYSTEM_BUS_ADDRESS") != nullptr;
}

static gboolean on_tick(gpointer data) {
    updates *u = data;
    xd_shell *shell = u->shell;
    int64_t now = g_get_real_time() / G_USEC_PER_SEC;
    int64_t mono = g_get_monotonic_time() / G_USEC_PER_SEC;
    if (u->started != 0 && mono - u->started > GIVE_UP)
        u->started = 0;
    if (turned_off(u) || u->started != 0)
        return G_SOURCE_CONTINUE;
    GTimeZone *tz = g_time_zone_new_local();
    int64_t due = xd_updates_next_check(&shell->settings.updates, u->st.checked, tz);
    g_time_zone_unref(tz);
    if (now >= due) {
        u->started = mono;
        xd_shell_launch(shell, xd_shell_own_command(shell, "xd-wuaucpl", "--check"));
    }
    return G_SOURCE_CONTINUE;
}

static gboolean first_tick(gpointer data) {
    updates *u = data;
    on_tick(u);
    u->tick = g_timeout_add_seconds(TICK_SECONDS, on_tick, u);
    return G_SOURCE_REMOVE;
}

/* ---- the icon ---------------------------------------------------------------------- */

static const char *icon(void *data) {
    updates *u = data;
    if (waiting(u) == 0 || turned_off(u))
        return nullptr;
    return u->st.important > 0 ? "software-update-urgent" : "software-update-available";
}

static char *tooltip(void *data) {
    updates *u = data;
    return g_strdup(u->st.downloaded || u->st.software == 0
                        ? "Updates are ready for your computer. Click here to install them."
                        : "Updates are available for your computer. Click here to review "
                          "them.");
}

static void activate(void *data, [[maybe_unused]] xd_taskbar *bar, [[maybe_unused]] xd_rect anchor,
                     [[maybe_unused]] uint32_t serial) {
    updates *u = data;
    xd_balloon_hide_tray(u->shell, ITEM);
    review(u->shell, nullptr);
}

static void open_updates(void *data) {
    updates *u = data;
    review(u->shell, nullptr);
}

static void menu([[maybe_unused]] void *data, xd_menu_tree *tree, xd_menu_model *m) {
    xd_menu_item *it = xd_menu_add(tree, m, XD_MENU_ACTION, "&Review Updates", nullptr);
    it->action = review;
    it->bold = true;
    it = xd_menu_add(tree, m, XD_MENU_ACTION, "Automatic Updates &Settings", nullptr);
    it->action = settings_page;
}

static void destroy(void *data) {
    updates *u = data;
    if (u->tick != 0)
        g_source_remove(u->tick);
    if (u->monitor != nullptr) {
        g_signal_handlers_disconnect_by_data(u->monitor, u);
        g_object_unref(u->monitor);
    }
    g_free(u->path);
    g_free(u);
}

static const xd_sysicon_ops ops = {
    .icon = icon,
    .tooltip = tooltip,
    .menu = menu,
    .activate = activate,
    .open = open_updates,
    .destroy = destroy,
};

void xd_sys_updates_init(xd_shell *shell) {
    if (!xd_shell_have_program("xd-wuaucpl")) {
        char *own = xd_shell_own_program("xd-wuaucpl");
        bool found = g_file_test(own, G_FILE_TEST_IS_EXECUTABLE);
        g_free(own);
        if (!found)
            return;
    }
    updates *u = g_new0(updates, 1);
    u->shell = shell;
    u->path = xd_updates_state_path();
    GFile *file = g_file_new_for_path(u->path);
    u->monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(file);
    if (u->monitor != nullptr)
        g_signal_connect(u->monitor, "changed", G_CALLBACK(on_changed), u);
    xd_tray_add_sysicon(shell, ITEM, &ops, u);
    load(u, true);
    if (!may_check())
        return;
    const char *test = getenv("XD_UPDATES_DELAY");
    int64_t delay = test != nullptr ? g_ascii_strtoll(test, nullptr, 10) : FIRST_DELAY;
    delay = XD_CLAMP(delay, 1, FIRST_DELAY);
    u->tick = g_timeout_add_seconds((unsigned)delay, first_tick, u);
}
