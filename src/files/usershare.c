/* SPDX-License-Identifier: MIT
 * Samba user shares. See usershare.h.
 */
#include "usershare.h"

#include <grp.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Everyone's SID: `net` takes it without asking smbd or winbind to look a
 * name up, which "Everyone" needs. */
#define EVERYONE_SID "S-1-1-0"

/* ---- running net and testparm ---------------------------------------------------- */

/* Runs `program` (net or testparm) with the test smb.conf if one is
 * given, in the C locale so its messages can be recognised. `out` and
 * `messages` get its stdout and stderr; FALSE if it couldn't run. */
static gboolean run(const char *program, const char *const *args, gboolean conf_last,
                    int *status, char **out, char **messages, GError **err) {
    const char *conf = g_getenv("XD_SMB_CONF");
    gboolean test_conf = conf != nullptr && conf[0] != '\0';
    GStrvBuilder *b = g_strv_builder_new();
    g_strv_builder_add(b, program);
    if (test_conf && !conf_last)
        g_strv_builder_add_many(b, "-s", conf, nullptr);
    for (; *args != nullptr; args++)
        g_strv_builder_add(b, *args);
    if (test_conf && conf_last)
        g_strv_builder_add(b, conf);
    GStrv argv = g_strv_builder_end(b);
    g_strv_builder_unref(b);
    GSubprocessLauncher *l = g_subprocess_launcher_new(
        (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
    g_subprocess_launcher_setenv(l, "LC_ALL", "C", TRUE);
    GSubprocess *p = g_subprocess_launcher_spawnv(l, (const char *const *)argv, err);
    g_object_unref(l);
    g_strfreev(argv);
    if (p == nullptr)
        return FALSE;
    char *o = nullptr, *e = nullptr;
    gboolean ok = g_subprocess_communicate_utf8(p, nullptr, nullptr, &o, &e, err);
    if (ok) {
        *status = g_subprocess_get_if_exited(p) ? g_subprocess_get_exit_status(p) : -1;
        *out = o != nullptr ? o : g_strdup("");
        if (messages != nullptr)
            *messages = g_strstrip(e != nullptr ? e : g_strdup(""));
        else
            g_free(e);
    }
    g_object_unref(p);
    return ok;
}

static gboolean have_net(void) {
    char *net = g_find_program_in_path("net");
    gboolean found = net != nullptr;
    g_free(net);
    return found;
}

/* ---- the shares ------------------------------------------------------------------- */

static void share_free(gpointer p) {
    XdUsershare *s = p;
    g_free(s->name);
    g_free(s->path);
    g_free(s->comment);
    g_free(s);
}

/* An ACL as `net` writes it, "S-1-1-0:F,": whether everyone may write. */
static gboolean acl_writable(const char *acl) {
    gboolean writable = FALSE;
    char **entries = g_strsplit(acl != nullptr ? acl : "", ",", -1);
    for (char **e = entries; *e != nullptr; e++) {
        const char *colon = strrchr(*e, ':');
        if (colon == nullptr)
            continue;
        char *who = g_strstrip(g_strndup(*e, (gsize)(colon - *e)));
        if ((strcmp(who, EVERYONE_SID) == 0 || g_ascii_strcasecmp(who, "Everyone") == 0) &&
            g_ascii_toupper(colon[1]) == 'F')
            writable = TRUE;
        g_free(who);
    }
    g_strfreev(entries);
    return writable;
}

GPtrArray *xd_usershare_parse_info(const char *text) {
    GPtrArray *out = g_ptr_array_new_with_free_func(share_free);
    GKeyFile *kf = g_key_file_new();
    if (text != nullptr &&
        g_key_file_load_from_data(kf, text, (gsize)-1, G_KEY_FILE_NONE, nullptr)) {
        char **groups = g_key_file_get_groups(kf, nullptr);
        for (char **g = groups; *g != nullptr; g++) {
            /* Raw values: a path's backslashes aren't escapes. */
            char *path = g_key_file_get_value(kf, *g, "path", nullptr);
            if (path == nullptr)
                continue;
            XdUsershare *s = g_new0(XdUsershare, 1);
            s->name = g_strdup(*g);
            s->path = path;
            s->comment = g_key_file_get_value(kf, *g, "comment", nullptr);
            if (s->comment == nullptr)
                s->comment = g_strdup("");
            char *acl = g_key_file_get_value(kf, *g, "usershare_acl", nullptr);
            s->writable = acl_writable(acl);
            g_free(acl);
            char *guest = g_key_file_get_value(kf, *g, "guest_ok", nullptr);
            s->guest_ok = guest != nullptr && g_ascii_tolower(guest[0]) == 'y';
            g_free(guest);
            g_ptr_array_add(out, s);
        }
        g_strfreev(groups);
    }
    g_key_file_free(kf);
    return out;
}

XdUsershareStatus xd_usershare_status_of(const char *messages) {
    if (messages == nullptr)
        return XD_USERSHARE_FAILED;
    if (strstr(messages, "usershares are currently disabled") != nullptr)
        return XD_USERSHARE_DISABLED;
    if (strstr(messages, "Permission denied") != nullptr ||
        strstr(messages, "cannot open usershare directory") != nullptr ||
        strstr(messages, "cannot create tmp file") != nullptr ||
        strstr(messages, "do not have permission") != nullptr)
        return XD_USERSHARE_NO_PERMISSION;
    return XD_USERSHARE_FAILED;
}

gboolean xd_usershare_dir_writable(const char *dir, char **group) {
    if (group != nullptr)
        *group = nullptr;
    if (access(dir, W_OK | X_OK) == 0)
        return TRUE;
    struct stat st;
    if (group != nullptr && stat(dir, &st) == 0) {
        struct group *gr = getgrgid(st.st_gid);
        if (gr != nullptr)
            *group = g_strdup(gr->gr_name);
    }
    return FALSE;
}

/* smb.conf's usershare path, by testparm; nullptr if unknown. */
static char *usershare_path(void) {
    char *testparm = g_find_program_in_path("testparm");
    if (testparm == nullptr)
        return nullptr;
    g_free(testparm);
    static const char *const args[] = {"-s", "--parameter-name=usershare path", nullptr};
    int status = 0;
    char *out = nullptr;
    if (!run("testparm", args, TRUE, &status, &out, nullptr, nullptr))
        return nullptr;
    g_strstrip(out);
    if (status != 0 || out[0] != '/')
        g_clear_pointer(&out, g_free);
    return out;
}

GPtrArray *xd_usershare_probe(XdUsershareStatus *status, char **detail) {
    *status = XD_USERSHARE_OK;
    if (detail != nullptr)
        *detail = nullptr;
    if (!have_net()) {
        *status = XD_USERSHARE_NO_SAMBA;
        return xd_usershare_parse_info(nullptr);
    }
    static const char *const args[] = {"usershare", "info", nullptr};
    int rc = 0;
    char *out = nullptr, *messages = nullptr;
    GError *err = nullptr;
    if (!run("net", args, FALSE, &rc, &out, &messages, &err)) {
        *status = XD_USERSHARE_FAILED;
        if (detail != nullptr)
            *detail = g_strdup(err->message);
        g_error_free(err);
        return xd_usershare_parse_info(nullptr);
    }
    GPtrArray *shares = xd_usershare_parse_info(rc == 0 ? out : nullptr);
    if (rc != 0) {
        *status = xd_usershare_status_of(messages);
        if (detail != nullptr)
            *detail = g_strdup(messages);
    } else {
        /* Listing works for anyone who can read the folder; making a
         * share needs writing to it. */
        char *dir = usershare_path();
        if (dir != nullptr && !xd_usershare_dir_writable(dir, nullptr)) {
            *status = XD_USERSHARE_NO_PERMISSION;
            if (detail != nullptr) {
                *detail = dir;
                dir = nullptr;
            }
        }
        g_free(dir);
    }
    g_free(out);
    g_free(messages);
    return shares;
}

const XdUsershare *xd_usershare_find(GPtrArray *shares, const char *path) {
    for (guint i = 0; shares != nullptr && i < shares->len; i++) {
        const XdUsershare *s = g_ptr_array_index(shares, i);
        if (g_strcmp0(s->path, path) == 0)
            return s;
    }
    return nullptr;
}

/* `net`'s failure as an error: its own message, else what's known. */
static gboolean net_failed(GError **err, const char *messages, const char *what) {
    g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                messages != nullptr && messages[0] != '\0' ? messages : what);
    return FALSE;
}

gboolean xd_usershare_add(const char *name, const char *path, const char *comment,
                          gboolean writable, GError **err) {
    const char *args[] = {"usershare",
                          "add",
                          name,
                          path,
                          comment != nullptr ? comment : "",
                          writable ? EVERYONE_SID ":F" : EVERYONE_SID ":R",
                          "guest_ok=n",
                          nullptr};
    int rc = 0;
    char *out = nullptr, *messages = nullptr;
    if (!run("net", args, FALSE, &rc, &out, &messages, err))
        return FALSE;
    gboolean ok = rc == 0 || net_failed(err, messages, "The folder could not be shared.");
    g_free(out);
    g_free(messages);
    if (ok)
        xd_usershare_refresh();
    return ok;
}

gboolean xd_usershare_delete(const char *name, GError **err) {
    const char *args[] = {"usershare", "delete", name, nullptr};
    int rc = 0;
    char *out = nullptr, *messages = nullptr;
    if (!run("net", args, FALSE, &rc, &out, &messages, err))
        return FALSE;
    gboolean ok = rc == 0 || net_failed(err, messages, "The folder could not be unshared.");
    g_free(out);
    g_free(messages);
    if (ok)
        xd_usershare_refresh();
    return ok;
}

const char *xd_usershare_name_problem(const char *name) {
    if (name == nullptr || name[0] == '\0')
        return "Type a share name.";
    if (g_utf8_strlen(name, -1) > 80)
        return "The share name is too long. It can't be longer than 80 characters.";
    /* Windows' reserved characters, and Samba's own (%). */
    if (strpbrk(name, "\"/\\[]:|<>+=;,?*%") != nullptr)
        return "The share name contains invalid characters.";
    for (const char *c = name; *c != '\0'; c++)
        if ((unsigned char)*c < 0x20)
            return "The share name contains invalid characters.";
    return nullptr;
}

char *xd_usershare_default_name(const char *path) {
    char *base = g_path_get_basename(path);
    char *display = g_filename_display_name(base);
    g_free(base);
    /* Characters a share name can't have become underscores. */
    g_strdelimit(display, "\"/\\[]:|<>+=;,?*%", '_');
    char *name = g_utf8_substring(display, 0, MIN(g_utf8_strlen(display, -1), 80));
    g_free(display);
    return name;
}

char *xd_usershare_help_text(XdUsershareStatus status, const char *detail) {
    switch (status) {
    case XD_USERSHARE_OK:
        return nullptr;
    case XD_USERSHARE_NO_SAMBA:
        return g_strdup("Sharing folders on the network needs Samba, which isn't installed. An "
                        "administrator can install the samba package.");
    case XD_USERSHARE_DISABLED:
        return g_strdup(
            "Sharing folders is turned off on this computer. To turn it on, an administrator "
            "must set \"usershare path\" (for example /var/lib/samba/usershares) and "
            "\"usershare max shares\" (for example 100) under [global] in /etc/samba/smb.conf, "
            "create that folder owned by a group such as sambashare with mode 1770, and add "
            "you to that group.");
    case XD_USERSHARE_NO_PERMISSION: {
        char *group = nullptr;
        if (detail != nullptr)
            xd_usershare_dir_writable(detail, &group);
        char *text = g_strdup_printf(
            "You aren't allowed to share folders: Samba keeps shares in %s, which you can't "
            "change. An administrator must add you to the group %s%s%s, and then you must log "
            "off and back on.",
            detail != nullptr ? detail : "a folder", group != nullptr ? "\"" : "",
            group != nullptr ? group : "that owns it", group != nullptr ? "\"" : "");
        g_free(group);
        return text;
    }
    case XD_USERSHARE_FAILED:
        return g_strdup_printf("Samba's sharing doesn't work: %s",
                               detail != nullptr && detail[0] != '\0' ? detail
                                                                      : "net usershare failed.");
    }
    return nullptr;
}

/* ---- the shared paths, for the overlay ------------------------------------------------- */

typedef struct watcher {
    void (*changed)(gpointer data);
    gpointer data;
} watcher;

static GHashTable *shared_paths;
static gint64 listed_at;
static gboolean listing, list_again;
static GSList *watchers;

static void start_listing(void);

static void listed(GObject *src, GAsyncResult *res, [[maybe_unused]] gpointer data) {
    GSubprocess *p = G_SUBPROCESS(src);
    char *out = nullptr;
    GHashTable *paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    if (g_subprocess_communicate_utf8_finish(p, res, &out, nullptr, nullptr) &&
        g_subprocess_get_if_exited(p) && g_subprocess_get_exit_status(p) == 0) {
        GPtrArray *shares = xd_usershare_parse_info(out);
        for (guint i = 0; i < shares->len; i++) {
            XdUsershare *s = g_ptr_array_index(shares, i);
            g_hash_table_add(paths, g_strdup(s->path));
        }
        g_ptr_array_unref(shares);
    }
    g_free(out);
    g_object_unref(p);
    gboolean changed = shared_paths == nullptr ||
                       g_hash_table_size(paths) != g_hash_table_size(shared_paths);
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, paths);
    while (!changed && g_hash_table_iter_next(&it, &key, nullptr))
        changed = !g_hash_table_contains(shared_paths, key);
    g_clear_pointer(&shared_paths, g_hash_table_unref);
    shared_paths = paths;
    listed_at = g_get_monotonic_time();
    listing = FALSE;
    if (changed)
        for (GSList *l = watchers; l != nullptr; l = l->next) {
            watcher *w = l->data;
            w->changed(w->data);
        }
    if (list_again) {
        list_again = FALSE;
        start_listing();
    }
}

static void start_listing(void) {
    if (listing) {
        list_again = TRUE;
        return;
    }
    if (!have_net()) {
        listed_at = g_get_monotonic_time();
        return;
    }
    const char *conf = g_getenv("XD_SMB_CONF");
    GSubprocessLauncher *l = g_subprocess_launcher_new(
        (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
    g_subprocess_launcher_setenv(l, "LC_ALL", "C", TRUE);
    GSubprocess *p = conf != nullptr && conf[0] != '\0'
                         ? g_subprocess_launcher_spawn(l, nullptr, "net", "-s", conf, "usershare",
                                                       "info", nullptr)
                         : g_subprocess_launcher_spawn(l, nullptr, "net", "usershare", "info",
                                                       nullptr);
    g_object_unref(l);
    if (p == nullptr) {
        listed_at = g_get_monotonic_time();
        return;
    }
    listing = TRUE;
    g_subprocess_communicate_utf8_async(p, nullptr, nullptr, listed, nullptr);
}

GHashTable *xd_usershare_shared_paths(void) {
    /* Shares change rarely, and only by `net`: a few seconds is fresh. */
    if (!listing && (listed_at == 0 || g_get_monotonic_time() - listed_at > 5 * G_USEC_PER_SEC))
        start_listing();
    return shared_paths;
}

void xd_usershare_watch(void (*changed)(gpointer data), gpointer data) {
    watcher *w = g_new(watcher, 1);
    w->changed = changed;
    w->data = data;
    watchers = g_slist_prepend(watchers, w);
}

void xd_usershare_unwatch(gpointer data) {
    for (GSList *l = watchers; l != nullptr; l = l->next) {
        watcher *w = l->data;
        if (w->data == data) {
            watchers = g_slist_delete_link(watchers, l);
            g_free(w);
            return;
        }
    }
}

void xd_usershare_refresh(void) {
    /* Only for those showing the hands (nobody may have asked yet). */
    if (watchers != nullptr || shared_paths != nullptr)
        start_listing();
}
