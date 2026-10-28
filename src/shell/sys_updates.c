/* SPDX-License-Identifier: MIT
 * Automatic Updates in the notification area, as XP's wuauclt did: on the
 * schedule in desktop.ini [Updates] (every day or one day a week at an
 * hour; a check missed while the computer was off runs soon after logon)
 * it runs `xd-wuaucpl --check`, which refreshes, counts and, in Download
 * mode, downloads, but never installs. What the check found is a state
 * file we watch: while updates wait there's an icon, and a new set of them
 * brings XP's balloon. A click on either opens the updates to install. A
 * check that fails says so in a balloon of its own, whose click opens the
 * installation history, where the check kept why.
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

static int waiting(const updates *auto_updates) {
    return auto_updates->st.software + auto_updates->st.firmware;
}

static bool turned_off(const updates *auto_updates) {
    return auto_updates->shell->settings.updates.mode == XD_UPDATES_OFF;
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

static void history(xd_shell *shell, [[maybe_unused]] void *data) {
    open_page(shell, "--history");
}

static void balloon(updates *auto_updates) {
    /* XP's words: "ready" once downloaded. */
    bool ready = auto_updates->st.downloaded || auto_updates->st.software == 0;
    xd_balloon_tray(
        auto_updates->shell, ITEM,
        auto_updates->st.important > 0 ? "software-update-urgent" : "software-update-available",
        ready ? "Updates are ready for your computer" : "Updates are available for your computer",
        ready ? "Click here to install these updates."
              : "Click here to review and install these updates.",
        review, nullptr);
}

static void load(updates *auto_updates, bool startup) {
    xd_updates_state old = auto_updates->st;
    if (!xd_updates_state_read(auto_updates->path, &auto_updates->st)) {
        auto_updates->st = (xd_updates_state){0};
        xd_tray_changed(auto_updates->shell);
        return;
    }
    auto_updates->loaded = true;
    if (auto_updates->st.checked != old.checked)
        auto_updates->started = 0; /* the check we started (or another) has ended */
    xd_tray_changed(auto_updates->shell);
    if (waiting(auto_updates) == 0 || turned_off(auto_updates)) {
        xd_balloon_hide_tray(auto_updates->shell, ITEM);
        auto_updates->shown[0] = '\0';
        return;
    }
    /* A check that just failed (the download, say) says so each time,
     * and a click shows why, from the installation history. */
    if (!startup && auto_updates->st.checked > old.checked && auto_updates->st.error[0] != '\0') {
        memcpy(auto_updates->shown, auto_updates->st.signature, sizeof(auto_updates->shown));
        bool download = g_str_has_prefix(auto_updates->st.error, "Downloading");
        xd_balloon_tray(auto_updates->shell, ITEM, "software-update-urgent",
                        download ? "Updates could not be downloaded"
                                 : "Your computer couldn't check for updates",
                        "Click here to see why.", history, nullptr);
        return;
    }
    /* At logon while updates wait, and when a check finds a new set; not
     * when the applet merely recounts them. */
    bool fresh = startup || (auto_updates->st.checked > old.checked &&
                             strcmp(auto_updates->st.signature, auto_updates->shown) != 0);
    if (fresh) {
        memcpy(auto_updates->shown, auto_updates->st.signature, sizeof(auto_updates->shown));
        balloon(auto_updates);
    }
}

static void on_changed([[maybe_unused]] GFileMonitor *file_monitor, [[maybe_unused]] GFile *file,
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
    updates *auto_updates = data;
    xd_shell *shell = auto_updates->shell;
    int64_t now = g_get_real_time() / G_USEC_PER_SEC;
    int64_t mono = g_get_monotonic_time() / G_USEC_PER_SEC;
    if (auto_updates->started != 0 && mono - auto_updates->started > GIVE_UP)
        auto_updates->started = 0;
    if (turned_off(auto_updates) || auto_updates->started != 0)
        return G_SOURCE_CONTINUE;
    GTimeZone *tz = g_time_zone_new_local();
    int64_t due = xd_updates_next_check(&shell->settings.updates, auto_updates->st.checked, tz);
    g_time_zone_unref(tz);
    if (now >= due) {
        auto_updates->started = mono;
        xd_shell_launch(shell, xd_shell_own_command(shell, "xd-wuaucpl", "--check"));
    }
    return G_SOURCE_CONTINUE;
}

static gboolean first_tick(gpointer data) {
    updates *auto_updates = data;
    on_tick(auto_updates);
    auto_updates->tick = g_timeout_add_seconds(TICK_SECONDS, on_tick, auto_updates);
    return G_SOURCE_REMOVE;
}

/* ---- the icon ---------------------------------------------------------------------- */

static const char *icon(void *data) {
    updates *auto_updates = data;
    if (waiting(auto_updates) == 0 || turned_off(auto_updates))
        return nullptr;
    return auto_updates->st.important > 0 ? "software-update-urgent" : "software-update-available";
}

static char *tooltip(void *data) {
    updates *auto_updates = data;
    return g_strdup(auto_updates->st.downloaded || auto_updates->st.software == 0
                        ? "Updates are ready for your computer. Click here to install them."
                        : "Updates are available for your computer. Click here to review "
                          "them.");
}

static void activate(void *data, [[maybe_unused]] xd_taskbar *bar, [[maybe_unused]] xd_rect anchor,
                     [[maybe_unused]] uint32_t serial) {
    updates *auto_updates = data;
    xd_balloon_hide_tray(auto_updates->shell, ITEM);
    review(auto_updates->shell, nullptr);
}

static void open_updates(void *data) {
    updates *auto_updates = data;
    review(auto_updates->shell, nullptr);
}

static void menu([[maybe_unused]] void *data, xd_menu_tree *tree, xd_menu_model *menu_model) {
    xd_menu_item *it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, "&Review Updates", nullptr);
    it->action = review;
    it->bold = true;
    it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, "Automatic Updates &Settings", nullptr);
    it->action = settings_page;
}

static void destroy(void *data) {
    updates *auto_updates = data;
    if (auto_updates->tick != 0)
        g_source_remove(auto_updates->tick);
    if (auto_updates->monitor != nullptr) {
        g_signal_handlers_disconnect_by_data(auto_updates->monitor, auto_updates);
        g_object_unref(auto_updates->monitor);
    }
    g_free(auto_updates->path);
    g_free(auto_updates);
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
    updates *auto_updates = g_new0(updates, 1);
    auto_updates->shell = shell;
    auto_updates->path = xd_updates_state_path();
    GFile *file = g_file_new_for_path(auto_updates->path);
    auto_updates->monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(file);
    if (auto_updates->monitor != nullptr)
        g_signal_connect(auto_updates->monitor, "changed", G_CALLBACK(on_changed), auto_updates);
    xd_tray_add_sysicon(shell, ITEM, &ops, auto_updates);
    load(auto_updates, true);
    if (!may_check())
        return;
    const char *test = getenv("XD_UPDATES_DELAY");
    int64_t delay = test != nullptr ? g_ascii_strtoll(test, nullptr, 10) : FIRST_DELAY;
    delay = XD_CLAMP(delay, 1, FIRST_DELAY);
    auto_updates->tick = g_timeout_add_seconds((unsigned)delay, first_tick, auto_updates);
}
