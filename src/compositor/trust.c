/* SPDX-License-Identifier: MIT
 * Who may use what. The shell, and a screen locker started again while
 * locked, run on a connection the compositor made for them (a socket
 * pair handed over as WAYLAND_SOCKET), so they're known by that
 * connection, not by a PID that could be reused or a program that could
 * be swapped in after connecting. Task Manager and input method programs,
 * started by others, are known by the installed program they ran when
 * they connected (a program that connects, then runs the real one,
 * doesn't pass), and not if they were made to load other code
 * (LD_PRELOAD).
 *
 * Sandboxed programs (Flatpak's, through security-context-v1) get none of
 * the protocols that reach past their own windows: capturing the screen
 * or the clipboard, managing others' windows, the monitors and their
 * colours, typing or pointing for the user, the desktop's own layers, or
 * locking the screen.
 */
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "../settings/inputmethod.h"
#include "server.h"

/* What a client was when it connected. */
typedef struct client_record {
    struct wl_listener destroy;
    bool task_manager; /* our installed xd-taskmgr */
    bool input_framework; /* an installed input method framework's program */
} client_record;

/* ---- trusted connections ------------------------------------------------------------ */

static void handle_trusted_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_trusted_client *trusted = XD_CONTAINER_OF(listener, xd_trusted_client, destroy);
    wl_list_remove(&trusted->destroy.link);
    wl_list_init(&trusted->destroy.link);
    trusted->client = nullptr;
}

bool xd_trust_is(const xd_trusted_client *trusted, const struct wl_client *client) {
    return trusted->client != nullptr && client != nullptr && trusted->client == client;
}

pid_t xd_trust_spawn(xd_server *server, char *const argv[], xd_trusted_client *trusted) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
        wlr_log_errno(WLR_ERROR, "cannot make a connection for %s", argv[0]);
        return -1;
    }
    struct wl_client *client = wl_client_create(server->display, pair[0]);
    if (client == nullptr) {
        close(pair[0]);
        close(pair[1]);
        return -1;
    }
    /* The child's end as fd 3 (dup2 drops close-on-exec), named by
     * WAYLAND_SOCKET, which libwayland takes (and unsets) on connecting. */
    size_t count = 0;
    while (environ[count] != nullptr)
        count++;
    char **envp = xd_alloc_array(count + 2, sizeof(*envp));
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(environ[i], "WAYLAND_SOCKET=", 15) != 0)
            envp[kept++] = environ[i];
    }
    char socket_var[] = "WAYLAND_SOCKET=3";
    envp[kept++] = socket_var;
    envp[kept] = nullptr;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pair[1], 3);
    pid_t pid = xd_spawn_with(argv, &actions, envp);
    posix_spawn_file_actions_destroy(&actions);
    xd_free(envp);
    close(pair[1]);
    if (pid < 0) {
        wl_client_destroy(client);
        return -1;
    }
    if (trusted->client != nullptr)
        wl_list_remove(&trusted->destroy.link);
    trusted->client = client;
    trusted->destroy.notify = handle_trusted_destroy;
    wl_client_add_destroy_listener(client, &trusted->destroy);
    return pid;
}

/* ---- what a client ran ----------------------------------------------------------------- */

/* The program `pid` runs, if it's still the installed file of that path
 * (not one deleted or replaced since, nor a sandbox's of that name). */
static bool installed_exe(pid_t pid, char *exe, size_t size) {
    char link[64];
    snprintf(link, sizeof(link), "/proc/%d/exe", (int)pid);
    ssize_t length = readlink(link, exe, size - 1);
    if (length <= 0)
        return false;
    exe[length] = '\0';
    struct stat running, installed;
    return stat(link, &running) == 0 && stat(exe, &installed) == 0 &&
           running.st_dev == installed.st_dev && running.st_ino == installed.st_ino;
}

/* Made to load code of someone else's choosing. */
static bool preloaded(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/environ", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return true; /* can't tell: not trusted */
    static const char *const loader[] = {"LD_PRELOAD=", "LD_AUDIT=", "LD_LIBRARY_PATH="};
    char buf[65536];
    ssize_t length = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (length < 0)
        return true;
    buf[length] = '\0';
    for (ssize_t at = 0; at < length; at += (ssize_t)strlen(buf + at) + 1) {
        for (size_t i = 0; i < XD_ARRAY_LEN(loader); i++) {
            if (strncmp(buf + at, loader[i], strlen(loader[i])) == 0 &&
                buf[at + (ssize_t)strlen(loader[i])] != '\0')
                return true;
        }
    }
    return false;
}

/* Our Task Manager: the xd-taskmgr installed beside this compositor. */
static bool runs_task_manager(const char *exe) {
    char *self = realpath("/proc/self/exe", nullptr);
    bool ok = false;
    char *slash = self != nullptr ? strrchr(self, '/') : nullptr;
    if (slash != nullptr) {
        slash[1] = '\0';
        size_t count = strlen(self);
        ok = strncmp(exe, self, count) == 0 && strcmp(exe + count, "xd-taskmgr") == 0;
    }
    free(self);
    return ok;
}

/* An installed input method framework's program, under /usr. */
static bool runs_input_framework(const char *exe) {
    const char *slash = strrchr(exe, '/');
    return strncmp(exe, "/usr/", 5) == 0 && slash != nullptr &&
           xd_im_program_is_framework(slash + 1);
}

static void handle_record_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    client_record *record = XD_CONTAINER_OF(listener, client_record, destroy);
    wl_list_remove(&record->destroy.link);
    xd_free(record);
}

static void handle_client_created(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, client_created);
    (void)server;
    struct wl_client *client = data;
    client_record *record = XD_NEW(client_record);
    pid_t pid = 0;
    wl_client_get_credentials(client, &pid, nullptr, nullptr);
    char exe[PATH_MAX];
    if (pid > 0 && pid != getpid() && installed_exe(pid, exe, sizeof(exe)) && !preloaded(pid)) {
        record->task_manager = runs_task_manager(exe);
        record->input_framework = runs_input_framework(exe);
    }
    record->destroy.notify = handle_record_destroy;
    wl_client_add_destroy_listener(client, &record->destroy);
}

static const client_record *record_of(const struct wl_client *client) {
    struct wl_listener *listener = wl_client_get_destroy_listener(
        (struct wl_client *)(uintptr_t)client, handle_record_destroy);
    return listener != nullptr ? XD_CONTAINER_OF(listener, client_record, destroy) : nullptr;
}

bool xd_trust_task_manager(const struct wl_client *client) {
    const client_record *record = record_of(client);
    return record != nullptr && record->task_manager;
}

bool xd_trust_input_framework(const struct wl_client *client) {
    const client_record *record = record_of(client);
    return record != nullptr && record->input_framework;
}

/* ---- sandboxes ---------------------------------------------------------------------- */

void xd_trust_privileged(xd_server *server, const struct wl_global *global) {
    if (global == nullptr)
        return;
    if (server->privileged_count == XD_ARRAY_LEN(server->privileged)) {
        wlr_log(WLR_ERROR, "trust: too many privileged globals");
        return;
    }
    server->privileged[server->privileged_count++] = global;
}

bool xd_trust_sandboxed_denied(const xd_server *server, const struct wl_client *client,
                               const struct wl_global *global) {
    if (server->security_context == nullptr ||
        wlr_security_context_manager_v1_lookup_client(server->security_context, client) == nullptr)
        return false;
    for (size_t i = 0; i < server->privileged_count; i++) {
        if (server->privileged[i] == global)
            return true;
    }
    return false;
}

void xd_trust_init(xd_server *server) {
    server->client_created.notify = handle_client_created;
    wl_display_add_client_created_listener(server->display, &server->client_created);
    server->security_context = wlr_security_context_manager_v1_create(server->display);
    wl_list_init(&server->shell_client.destroy.link);
    wl_list_init(&server->locker_client.destroy.link);
}
