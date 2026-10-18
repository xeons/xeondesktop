/* SPDX-License-Identifier: MIT
 * Which session the shell runs in: xd-compositor's, or another
 * compositor's (--taskbar-only, xeondesktop-taskbar.service), told by the
 * compositor at the other end of the Wayland connection rather than by
 * the environment alone; and the Logon sound, once a session.
 */
#include <fcntl.h>
#include <glib.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../settings/soundplay.h"
#include "shell.h"

pid_t xd_session_compositor_pid(struct wl_display *display) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(wl_display_get_fd(display), SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
        len != sizeof(cred))
        return 0;
    return cred.pid > 0 ? cred.pid : 0;
}

/* XEONDESKTOP_PID alone can't say: the user manager's environment can
 * keep it from an earlier XeonDesktop session, and a shell started from a
 * XeonDesktop terminal into another compositor inherits it. A stale one
 * would have programs started from here (Log Off in xd-shutdown, Windows
 * Security's logon time) act on a compositor that isn't ours, so it goes,
 * and XEONDESKTOP_NESTED, which came with it. Under another compositor
 * the session is real unless the caller says otherwise
 * (XEONDESKTOP_NESTED=1 without a XEONDESKTOP_PID). */
void xd_session_detect(xd_shell *shell, pid_t compositor) {
    const char *env = getenv("XEONDESKTOP_PID");
    int64_t pid = 0;
    bool valid = env != nullptr && xd_str_parse_i64(xd_str_from_cstr(env), 2, INT32_MAX, &pid);
    if (valid && (compositor == 0 || pid == compositor))
        return; /* xd-compositor's shell */
    shell->standalone = true;
    if (env != nullptr || getenv("XEONDESKTOP_NESTED") == nullptr)
        setenv("XEONDESKTOP_NESTED", "0", true);
    unsetenv("XEONDESKTOP_PID");
}

/* A shell that crashed and was restarted (by xd-compositor or by
 * systemd) mustn't greet the user again. A session is the compositor's
 * process, told apart from a later one with the same pid by its start
 * time; the marker is in XDG_RUNTIME_DIR, which logging out clears. */
void xd_session_logon_sound(pid_t compositor) {
    if (compositor == 0)
        return;
    char *stat_path = g_strdup_printf("/proc/%d/stat", (int)compositor);
    char *stat = nullptr;
    char **fields = nullptr;
    if (g_file_get_contents(stat_path, &stat, nullptr, nullptr)) {
        /* Field 22, starttime, counts from the ")" that ends the command
         * name (which may hold spaces). */
        const char *end = strrchr(stat, ')');
        if (end != nullptr && end[1] == ' ')
            fields = g_strsplit(end + 2, " ", 0);
    }
    if (fields != nullptr && g_strv_length(fields) > 19) {
        char *name = g_strdup_printf("xeondesktop-logon-%d-%s", (int)compositor, fields[19]);
        char *path = g_build_filename(g_get_user_runtime_dir(), name, nullptr);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            close(fd);
            xd_sound_play_event(XD_SOUND_LOGON);
        }
        g_free(path);
        g_free(name);
    }
    g_strfreev(fields);
    g_free(stat);
    g_free(stat_path);
}
