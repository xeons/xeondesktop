/* SPDX-License-Identifier: MIT
 * Network drives and places. See netdrives.h.
 */
#include "netdrives.h"

#include <glib/gstdio.h>
#include <string.h>

/* ---- folders --------------------------------------------------------------------- */

static void invalid(GError **err, const char *text) {
    g_set_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "\"%s\" is not a network folder. Type it as \\\\server\\share, or as an "
                "address such as smb://server/share or nfs://server/path.",
                text);
}

/* The path without trailing slashes ("/" for none). */
static char *trim_path(const char *path) {
    char *trimmed = g_strdup(path != nullptr && path[0] != '\0' ? path : "/");
    size_t count = strlen(trimmed);
    while (count > 1 && trimmed[count - 1] == '/')
        trimmed[--count] = '\0';
    if (trimmed[0] != '/') {
        char *rooted = g_strconcat("/", trimmed, nullptr);
        g_free(trimmed);
        trimmed = rooted;
    }
    return trimmed;
}

/* Rebuilt from its (unescaped) parts, so equal folders read the same. */
static char *join(const char *scheme, const char *userinfo, const char *host, int port,
                  const char *path) {
    char *trimmed = trim_path(path);
    char *lower = g_ascii_strdown(scheme, -1);
    char *uri = g_uri_join(G_URI_FLAGS_NONE, lower, userinfo, host, port, trimmed, nullptr,
                           nullptr);
    g_free(lower);
    g_free(trimmed);
    return uri;
}

/* Network kinds whose addresses name a server; other kinds GIO mounts
 * (gvfs's localtest, which the tests use) may have none. Local places are
 * never network folders. */
static gboolean needs_host(const char *scheme) {
    static const char *const hosted[] = {"smb", "nfs",  "ftp",  "ftps",  "sftp", "ssh",
                                         "dav", "davs", "afp",  "http",  "https"};
    for (size_t i = 0; i < G_N_ELEMENTS(hosted); i++)
        if (g_ascii_strcasecmp(scheme, hosted[i]) == 0)
            return TRUE;
    return FALSE;
}

static gboolean local_scheme(const char *scheme) {
    static const char *const local[] = {"file",    "trash",  "computer", "control",
                                        "network", "recent", "burn",     "admin"};
    for (size_t i = 0; i < G_N_ELEMENTS(local); i++)
        if (g_ascii_strcasecmp(scheme, local[i]) == 0)
            return TRUE;
    return FALSE;
}

char *xd_net_parse_folder(const char *text, gboolean need_share, GError **err) {
    g_return_val_if_fail(text != nullptr, nullptr);
    char *trimmed = g_strstrip(g_strdup(text));
    char *out = nullptr;
    if (trimmed[0] == '\\' && trimmed[1] == '\\') {
        /* \\server\share\dir: Windows' own way to write a share. Forward
         * slashes work there too. */
        g_strdelimit(trimmed, "/", '\\');
        char **parts = g_strsplit(trimmed + 2, "\\", -1);
        GString *path = g_string_new(nullptr);
        for (guint i = 1; parts[0] != nullptr && parts[i] != nullptr; i++) {
            if (parts[i][0] == '\0')
                continue;
            g_string_append_c(path, '/');
            g_string_append(path, parts[i]);
        }
        const char *host = parts[0];
        gboolean host_ok = host != nullptr && host[0] != '\0' && strpbrk(host, " :@") == nullptr;
        if (host_ok && (!need_share || path->len > 0))
            out = join("smb", nullptr, host, -1, path->str);
        g_string_free(path, TRUE);
        g_strfreev(parts);
    } else if (g_uri_peek_scheme(trimmed) != nullptr) {
        GUri *uri = g_uri_parse(trimmed, G_URI_FLAGS_PARSE_RELAXED, nullptr);
        const char *scheme = uri != nullptr ? g_uri_get_scheme(uri) : nullptr;
        const char *host = uri != nullptr ? g_uri_get_host(uri) : nullptr;
        gboolean has_host = host != nullptr && host[0] != '\0';
        if (scheme != nullptr && !local_scheme(scheme) && (has_host || !needs_host(scheme))) {
            char *path = trim_path(g_uri_get_path(uri));
            if (!need_share || strcmp(path, "/") != 0)
                out = join(scheme, g_uri_get_userinfo(uri), has_host ? host : "",
                           g_uri_get_port(uri), path);
            g_free(path);
        }
        g_clear_pointer(&uri, g_uri_unref);
    }
    if (out == nullptr)
        invalid(err, trimmed);
    g_free(trimmed);
    return out;
}

/* A URI's parts, unescaped. */
typedef struct parts {
    char *scheme, *host, *path;
} parts;

static void parts_clear(parts *split_uri) {
    g_free(split_uri->scheme);
    g_free(split_uri->host);
    g_free(split_uri->path);
}

/* FALSE (and nothing to clear) if it doesn't parse. */
static gboolean split(const char *uri, parts *out) {
    GUri *parsed = uri != nullptr ? g_uri_parse(uri, G_URI_FLAGS_PARSE_RELAXED, nullptr) : nullptr;
    if (parsed == nullptr)
        return FALSE;
    out->scheme = g_ascii_strdown(g_uri_get_scheme(parsed), -1);
    out->host = g_strdup(g_uri_get_host(parsed) != nullptr ? g_uri_get_host(parsed) : "");
    out->path = trim_path(g_uri_get_path(parsed));
    g_uri_unref(parsed);
    return TRUE;
}

gboolean xd_net_same_uri(const char *left, const char *right) {
    parts pa, pb;
    if (!split(left, &pa))
        return FALSE;
    if (!split(right, &pb)) {
        parts_clear(&pa);
        return FALSE;
    }
    /* Host names don't care about case; SMB's paths don't either. */
    gboolean same =
        strcmp(pa.scheme, pb.scheme) == 0 && g_ascii_strcasecmp(pa.host, pb.host) == 0;
    if (same && strcmp(pa.scheme, "smb") == 0) {
        char *fa = g_utf8_casefold(pa.path, -1);
        char *fb = g_utf8_casefold(pb.path, -1);
        same = strcmp(fa, fb) == 0;
        g_free(fa);
        g_free(fb);
    } else if (same) {
        same = strcmp(pa.path, pb.path) == 0;
    }
    parts_clear(&pa);
    parts_clear(&pb);
    return same;
}

char *xd_net_display_path(const char *uri) {
    parts split_uri;
    if (!split(uri, &split_uri))
        return g_strdup(uri != nullptr ? uri : "");
    char *out;
    if (strcmp(split_uri.scheme, "smb") == 0) {
        g_strdelimit(split_uri.path, "/", '\\');
        out = strcmp(split_uri.path, "\\") == 0
                  ? g_strconcat("\\\\", split_uri.host, nullptr)
                  : g_strconcat("\\\\", split_uri.host, split_uri.path, nullptr);
    } else {
        out = g_uri_unescape_string(uri, nullptr);
        if (out == nullptr)
            out = g_strdup(uri);
    }
    parts_clear(&split_uri);
    return out;
}

char *xd_net_drive_name(const char *uri) {
    parts split_uri;
    if (!split(uri, &split_uri))
        return g_strdup(uri != nullptr ? uri : "");
    const char *last = strrchr(split_uri.path, '/');
    last = last != nullptr ? last + 1 : split_uri.path;
    char *out = last[0] != '\0' && split_uri.host[0] != '\0'
                    ? g_strdup_printf("%s on '%s'", last, split_uri.host)
                : last[0] != '\0'           ? g_strdup(last)
                : split_uri.host[0] != '\0' ? g_strdup(split_uri.host)
                                            : g_strdup(uri);
    parts_clear(&split_uri);
    return out;
}

char *xd_net_host(const char *uri) {
    parts split_uri;
    if (!split(uri, &split_uri))
        return nullptr;
    char *host = split_uri.host[0] != '\0' ? g_strdup(split_uri.host) : nullptr;
    parts_clear(&split_uri);
    return host;
}

gboolean xd_net_scheme_supported(const char *scheme) {
    const gchar *const *schemes = g_vfs_get_supported_uri_schemes(g_vfs_get_default());
    for (; scheme != nullptr && schemes != nullptr && *schemes != nullptr; schemes++)
        if (g_ascii_strcasecmp(*schemes, scheme) == 0)
            return TRUE;
    return FALSE;
}

char *xd_net_unsupported_text(const char *uri) {
    char *scheme = uri != nullptr ? g_uri_parse_scheme(uri) : nullptr;
    char *text;
    if (g_strcmp0(scheme, "smb") == 0)
        text = g_strdup("Windows network folders can't be opened: GIO's support for them "
                        "(gvfs's SMB backend) isn't installed.\n\nAn administrator can install "
                        "the gvfs-smb package (gvfs-backends on Debian and Ubuntu). Seeing the "
                        "computers of a workgroup also needs WS-Discovery (gvfs-wsdd on Arch), "
                        "as newer Windows computers no longer announce themselves otherwise.");
    else if (g_strcmp0(scheme, "nfs") == 0)
        text = g_strdup("NFS folders can't be opened: GIO's support for them (gvfs's NFS "
                        "backend) isn't installed.\n\nAn administrator can install the gvfs-nfs "
                        "package (gvfs-backends on Debian and Ubuntu).");
    else
        text = g_strdup_printf("Folders at %s:// addresses can't be opened: GIO has no support "
                               "for them installed (a gvfs backend).",
                               scheme != nullptr ? scheme : "these");
    g_free(scheme);
    return text;
}

/* ---- the list -------------------------------------------------------------------- */

#define DRIVE_PREFIX "Drive "
#define PLACE_PREFIX "Place "

char *xd_net_config_path(void) {
    return g_build_filename(g_get_user_config_dir(), "xeondesktop", "network.ini", nullptr);
}

static GKeyFile *load(const char *path) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, nullptr);
    return kf;
}

static gboolean save(GKeyFile *kf, const char *path, GError **err) {
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    return g_key_file_save_to_file(kf, path, err); /* written whole, then renamed */
}

/* The group of the entry for `uri` among those named `prefix` N, or
 * nullptr. Free with g_free. */
static char *find_group(GKeyFile *kf, const char *prefix, const char *uri) {
    char **groups = g_key_file_get_groups(kf, nullptr);
    char *found = nullptr;
    for (char **group_name = groups; found == nullptr && *group_name != nullptr; group_name++) {
        if (!g_str_has_prefix(*group_name, prefix))
            continue;
        char *group_uri = g_key_file_get_string(kf, *group_name, "URI", nullptr);
        if (group_uri != nullptr && xd_net_same_uri(group_uri, uri))
            found = g_strdup(*group_name);
        g_free(group_uri);
    }
    g_strfreev(groups);
    return found;
}

/* "Drive N" with N one past the highest there. */
static char *new_group(GKeyFile *kf, const char *prefix) {
    char **groups = g_key_file_get_groups(kf, nullptr);
    guint64 top = 0;
    for (char **group_name = groups; *group_name != nullptr; group_name++) {
        guint64 number = 0;
        if (g_str_has_prefix(*group_name, prefix) &&
            g_ascii_string_to_unsigned(*group_name + strlen(prefix), 10, 1, G_MAXUINT32, &number,
                                       nullptr))
            top = MAX(top, number);
    }
    g_strfreev(groups);
    return g_strdup_printf("%s%" G_GUINT64_FORMAT, prefix, top + 1);
}

static void drive_free(gpointer user_data) {
    XdNetDrive *net_drive = user_data;
    g_free(net_drive->uri);
    g_free(net_drive->user);
    g_free(net_drive);
}

static void place_free(gpointer user_data) {
    XdNetPlace *pl = user_data;
    g_free(pl->uri);
    g_free(pl->name);
    g_free(pl);
}

GPtrArray *xd_net_drives_load(const char *path) {
    GPtrArray *out = g_ptr_array_new_with_free_func(drive_free);
    GKeyFile *kf = load(path);
    char **groups = g_key_file_get_groups(kf, nullptr);
    for (char **group_name = groups; *group_name != nullptr; group_name++) {
        if (!g_str_has_prefix(*group_name, DRIVE_PREFIX))
            continue;
        char *uri = g_key_file_get_string(kf, *group_name, "URI", nullptr);
        if (uri == nullptr || uri[0] == '\0') {
            g_free(uri);
            continue;
        }
        XdNetDrive *net_drive = g_new0(XdNetDrive, 1);
        net_drive->uri = uri;
        net_drive->user = g_key_file_get_string(kf, *group_name, "User", nullptr);
        if (net_drive->user != nullptr && net_drive->user[0] == '\0')
            g_clear_pointer(&net_drive->user, g_free);
        net_drive->reconnect = g_key_file_get_boolean(kf, *group_name, "Reconnect", nullptr);
        g_ptr_array_add(out, net_drive);
    }
    g_strfreev(groups);
    g_key_file_free(kf);
    return out;
}

GPtrArray *xd_net_places_load(const char *path) {
    GPtrArray *out = g_ptr_array_new_with_free_func(place_free);
    GKeyFile *kf = load(path);
    char **groups = g_key_file_get_groups(kf, nullptr);
    for (char **group_name = groups; *group_name != nullptr; group_name++) {
        if (!g_str_has_prefix(*group_name, PLACE_PREFIX))
            continue;
        char *uri = g_key_file_get_string(kf, *group_name, "URI", nullptr);
        if (uri == nullptr || uri[0] == '\0') {
            g_free(uri);
            continue;
        }
        XdNetPlace *pl = g_new0(XdNetPlace, 1);
        pl->uri = uri;
        pl->name = g_key_file_get_string(kf, *group_name, "Name", nullptr);
        if (pl->name == nullptr || pl->name[0] == '\0') {
            g_free(pl->name);
            pl->name = xd_net_drive_name(uri);
        }
        g_ptr_array_add(out, pl);
    }
    g_strfreev(groups);
    g_key_file_free(kf);
    return out;
}

gboolean xd_net_drive_add(const char *path, const char *uri, const char *user,
                          gboolean reconnect, GError **err) {
    GKeyFile *kf = load(path);
    char *group = find_group(kf, DRIVE_PREFIX, uri);
    if (group == nullptr)
        group = new_group(kf, DRIVE_PREFIX);
    g_key_file_set_string(kf, group, "URI", uri);
    g_key_file_set_boolean(kf, group, "Reconnect", reconnect);
    if (user != nullptr && user[0] != '\0')
        g_key_file_set_string(kf, group, "User", user);
    else
        g_key_file_remove_key(kf, group, "User", nullptr);
    gboolean ok = save(kf, path, err);
    g_free(group);
    g_key_file_free(kf);
    return ok;
}

static gboolean remove_entry(const char *path, const char *prefix, const char *uri,
                             GError **err) {
    GKeyFile *kf = load(path);
    gboolean changed = FALSE;
    char *group;
    while ((group = find_group(kf, prefix, uri)) != nullptr) {
        g_key_file_remove_group(kf, group, nullptr);
        g_free(group);
        changed = TRUE;
    }
    gboolean ok = !changed || save(kf, path, err);
    g_key_file_free(kf);
    return ok;
}

gboolean xd_net_drive_remove(const char *path, const char *uri, GError **err) {
    return remove_entry(path, DRIVE_PREFIX, uri, err);
}

gboolean xd_net_place_add(const char *path, const char *uri, const char *name, GError **err) {
    GKeyFile *kf = load(path);
    char *group = find_group(kf, PLACE_PREFIX, uri);
    if (group == nullptr)
        group = new_group(kf, PLACE_PREFIX);
    g_key_file_set_string(kf, group, "URI", uri);
    g_key_file_set_string(kf, group, "Name", name != nullptr ? name : "");
    gboolean ok = save(kf, path, err);
    g_free(group);
    g_key_file_free(kf);
    return ok;
}

gboolean xd_net_place_remove(const char *path, const char *uri, GError **err) {
    return remove_entry(path, PLACE_PREFIX, uri, err);
}

gboolean xd_net_forget_session_drives(const char *path, GError **err) {
    GKeyFile *kf = load(path);
    char **groups = g_key_file_get_groups(kf, nullptr);
    gboolean changed = FALSE;
    for (char **group_name = groups; *group_name != nullptr; group_name++) {
        if (g_str_has_prefix(*group_name, DRIVE_PREFIX) &&
            !g_key_file_get_boolean(kf, *group_name, "Reconnect", nullptr)) {
            g_key_file_remove_group(kf, *group_name, nullptr);
            changed = TRUE;
        }
    }
    g_strfreev(groups);
    gboolean ok = !changed || save(kf, path, err);
    g_key_file_free(kf);
    return ok;
}

/* ---- answering gvfs ---------------------------------------------------------------- */

void xd_net_login_reply(GMountOperation *op, GAskPasswordFlags flags, const char *user,
                        const char *password, gboolean remember) {
    const char *user_name = user != nullptr ? user : "";
    if (user_name[0] == '\0' && (flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED)) {
        g_mount_operation_set_anonymous(op, TRUE);
    } else {
        g_mount_operation_set_anonymous(op, FALSE);
        const char *slash = strchr(user_name, '\\');
        if (slash != nullptr && (flags & G_ASK_PASSWORD_NEED_DOMAIN)) {
            char *domain = g_strndup(user_name, (gsize)(slash - user_name));
            g_mount_operation_set_domain(op, domain);
            g_free(domain);
            user_name = slash + 1;
        }
        g_mount_operation_set_username(op, user_name);
        g_mount_operation_set_password(op, password != nullptr ? password : "");
    }
    g_mount_operation_set_password_save(op, remember && (flags & G_ASK_PASSWORD_SAVING_SUPPORTED)
                                                ? G_PASSWORD_SAVE_PERMANENTLY
                                                : G_PASSWORD_SAVE_NEVER);
    g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
}

static void free_secret(gpointer user_data) {
    if (user_data != nullptr)
        memset(user_data, 0, strlen(user_data));
    g_free(user_data);
}

void xd_net_operation_preset(GMountOperation *op, const char *user, const char *password) {
    g_object_set_data_full(G_OBJECT(op), "xd-net-user", g_strdup(user), g_free);
    g_object_set_data_full(G_OBJECT(op), "xd-net-password", g_strdup(password), free_secret);
    g_object_set_data(G_OBJECT(op), "xd-net-tried", nullptr);
}

gboolean xd_net_answer_preset(GMountOperation *op, GAskPasswordFlags flags) {
    const char *user = g_object_get_data(G_OBJECT(op), "xd-net-user");
    if (user == nullptr || g_object_get_data(G_OBJECT(op), "xd-net-tried") != nullptr)
        return FALSE;
    g_object_set_data(G_OBJECT(op), "xd-net-tried", GINT_TO_POINTER(1));
    xd_net_login_reply(op, flags, user, g_object_get_data(G_OBJECT(op), "xd-net-password"),
                       FALSE);
    return TRUE;
}

/* Each handler stops the signal: GMountOperation's own handler would
 * reply again ("unhandled") from an idle. */
static void quiet_password(GMountOperation *op, [[maybe_unused]] const char *message,
                           [[maybe_unused]] const char *user, [[maybe_unused]] const char *domain,
                           GAskPasswordFlags flags) {
    g_signal_stop_emission_by_name(op, "ask-password");
    if (!xd_net_answer_preset(op, flags))
        g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
}

static void quiet_question(GMountOperation *op, [[maybe_unused]] const char *message,
                           [[maybe_unused]] const char **choices) {
    g_signal_stop_emission_by_name(op, "ask-question");
    g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
}

GMountOperation *xd_net_quiet_operation_new(void) {
    GMountOperation *op = g_mount_operation_new();
    g_signal_connect(op, "ask-password", G_CALLBACK(quiet_password), nullptr);
    g_signal_connect(op, "ask-question", G_CALLBACK(quiet_question), nullptr);
    return op;
}

/* ---- logon --------------------------------------------------------------------------- */

typedef struct reconnect_job {
    guint pending, connected, failed;
    void (*done)(guint connected, guint failed, gpointer data);
    gpointer data;
} reconnect_job;

typedef struct reconnect_one {
    reconnect_job *job;
    GCancellable *cancel;
    guint timeout;
    char *uri;
} reconnect_one;

static void job_step(reconnect_job *job) {
    if (--job->pending > 0)
        return;
    if (job->done != nullptr)
        job->done(job->connected, job->failed, job->data);
    g_free(job);
}

static gboolean reconnect_timeout(gpointer data) {
    reconnect_one *one = data;
    one->timeout = 0;
    g_cancellable_cancel(one->cancel);
    return G_SOURCE_REMOVE;
}

static void reconnected(GObject *src, GAsyncResult *res, gpointer data) {
    reconnect_one *one = data;
    GError *err = nullptr;
    if (g_file_mount_enclosing_volume_finish(G_FILE(src), res, &err) ||
        g_error_matches(err, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
        one->job->connected++;
    } else {
        /* XP showed these with a red X, and a balloon at most. */
        g_printerr("xd-files: could not reconnect %s: %s\n", one->uri, err->message);
        one->job->failed++;
    }
    g_clear_error(&err);
    if (one->timeout != 0)
        g_source_remove(one->timeout);
    g_object_unref(one->cancel);
    reconnect_job *job = one->job;
    g_free(one->uri);
    g_free(one);
    job_step(job);
}

void xd_net_reconnect_all(const char *path, guint timeout_s,
                          void (*done)(guint connected, guint failed, gpointer data),
                          gpointer data) {
    GError *err = nullptr;
    if (!xd_net_forget_session_drives(path, &err)) {
        g_printerr("xd-files: %s\n", err->message);
        g_clear_error(&err);
    }
    reconnect_job *job = g_new0(reconnect_job, 1);
    job->done = done;
    job->data = data;
    job->pending = 1; /* until all are started */
    GPtrArray *drives = xd_net_drives_load(path);
    for (guint i = 0; i < drives->len; i++) {
        XdNetDrive *net_drive = g_ptr_array_index(drives, i);
        char *scheme = g_uri_parse_scheme(net_drive->uri);
        gboolean supported = scheme != nullptr && xd_net_scheme_supported(scheme);
        g_free(scheme);
        if (!supported) {
            g_printerr("xd-files: could not reconnect %s: GIO can't open it here\n",
                       net_drive->uri);
            job->failed++;
            continue;
        }
        reconnect_one *one = g_new0(reconnect_one, 1);
        one->job = job;
        one->uri = g_strdup(net_drive->uri);
        one->cancel = g_cancellable_new();
        one->timeout = g_timeout_add_seconds(timeout_s, reconnect_timeout, one);
        job->pending++;
        GFile *file = g_file_new_for_uri(net_drive->uri);
        GMountOperation *op = xd_net_quiet_operation_new();
        g_file_mount_enclosing_volume(file, G_MOUNT_MOUNT_NONE, op, one->cancel, reconnected, one);
        g_object_unref(op);
        g_object_unref(file);
    }
    g_ptr_array_unref(drives);
    job_step(job);
}
