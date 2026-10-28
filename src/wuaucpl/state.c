/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "../core/common.h"
#include "../settings/state.h"
#include "state.h"

static constexpr char GROUP[] = "Updates";

int64_t xd_updates_next_check(const xd_update_settings *update_settings, int64_t last,
                              GTimeZone *tz) {
    if (update_settings->mode == XD_UPDATES_OFF)
        return INT64_MAX;
    if (last <= 0)
        return 0;
    GDateTime *utc = g_date_time_new_from_unix_utc(last);
    GDateTime *at = utc != nullptr ? g_date_time_to_timezone(utc, tz) : nullptr;
    if (utc != nullptr)
        g_date_time_unref(utc);
    if (at == nullptr)
        return 0;
    /* GLib counts Monday as 1 and Sunday as 7; Day counts Sunday as 1. */
    int want = update_settings->day == 0   ? 0
               : update_settings->day == 1 ? 7
                                           : update_settings->day - 1;
    int64_t due = last + 24 * 3600;
    GDateTime *midnight = g_date_time_new(tz, g_date_time_get_year(at),
                                          g_date_time_get_month(at),
                                          g_date_time_get_day_of_month(at), 0, 0, 0);
    /* Today's time if it's still to come, else the next day (of the
     * week) with it; a week and a day covers every case. */
    for (int i = 0; midnight != nullptr && i <= 8; i++) {
        GDateTime *day = g_date_time_add_days(midnight, i);
        GDateTime *slot =
            g_date_time_new(tz, g_date_time_get_year(day), g_date_time_get_month(day),
                            g_date_time_get_day_of_month(day), update_settings->hour, 0, 0);
        bool found = slot != nullptr && g_date_time_to_unix(slot) > last &&
                     (want == 0 || g_date_time_get_day_of_week(slot) == want);
        if (found)
            due = g_date_time_to_unix(slot);
        if (slot != nullptr)
            g_date_time_unref(slot);
        g_date_time_unref(day);
        if (found)
            break;
    }
    if (midnight != nullptr)
        g_date_time_unref(midnight);
    g_date_time_unref(at);
    return due;
}

char *xd_updates_state_path(void) {
    xd_strbuf path = {0};
    xd_state_path("updates.ini", &path);
    char *out = g_strdup(xd_strbuf_cstr(&path));
    xd_strbuf_free(&path);
    return out;
}

static int get_count(GKeyFile *kf, const char *key) {
    int count = g_key_file_get_integer(kf, GROUP, key, nullptr);
    return XD_CLAMP(count, 0, 1000000);
}

bool xd_updates_state_read(const char *path, xd_updates_state *st) {
    *st = (xd_updates_state){0};
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, nullptr)) {
        g_key_file_free(kf);
        return false;
    }
    st->checked = g_key_file_get_int64(kf, GROUP, "Checked", nullptr);
    if (st->checked < 0)
        st->checked = 0;
    st->software = get_count(kf, "Software");
    st->firmware = get_count(kf, "Firmware");
    st->important = get_count(kf, "Important");
    st->downloaded = g_key_file_get_boolean(kf, GROUP, "Downloaded", nullptr);
    st->metered = g_key_file_get_boolean(kf, GROUP, "Metered", nullptr);
    char *sig = g_key_file_get_string(kf, GROUP, "Signature", nullptr);
    /* 40 hex digits or nothing. */
    if (sig != nullptr && strlen(sig) == 40 && strspn(sig, "0123456789abcdef") == 40)
        memcpy(st->signature, sig, 41);
    g_free(sig);
    char *error = g_key_file_get_string(kf, GROUP, "Error", nullptr);
    if (error != nullptr)
        g_strlcpy(st->error, error, sizeof(st->error));
    g_free(error);
    g_key_file_free(kf);
    return true;
}

bool xd_updates_state_write(const char *path, const xd_updates_state *st) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_int64(kf, GROUP, "Checked", st->checked);
    g_key_file_set_integer(kf, GROUP, "Software", st->software);
    g_key_file_set_integer(kf, GROUP, "Firmware", st->firmware);
    g_key_file_set_integer(kf, GROUP, "Important", st->important);
    g_key_file_set_boolean(kf, GROUP, "Downloaded", st->downloaded);
    g_key_file_set_boolean(kf, GROUP, "Metered", st->metered);
    g_key_file_set_string(kf, GROUP, "Signature", st->signature);
    g_key_file_set_string(kf, GROUP, "Error", st->error);
    gsize len = 0;
    char *data = g_key_file_to_data(kf, &len, nullptr);
    /* Written to a temporary file and renamed over it. */
    bool ok = g_file_set_contents(path, data, (gssize)len, nullptr);
    g_free(data);
    g_key_file_free(kf);
    return ok;
}

static int compare_ids(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

void xd_updates_signature(const char *const *ids, size_t count, char out[41]) {
    out[0] = '\0';
    if (count == 0)
        return;
    const char **sorted = g_new(const char *, count);
    memcpy(sorted, ids, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), compare_ids);
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA1);
    for (size_t i = 0; i < count; i++)
        g_checksum_update(sum, (const guchar *)sorted[i], (gssize)strlen(sorted[i]) + 1);
    g_strlcpy(out, g_checksum_get_string(sum), 41);
    g_checksum_free(sum);
    g_free(sorted);
}

/* ---- the installation history ---------------------------------------------------- */

static const char *const action_keys[XD_UPDATE_ACTION_COUNT] = {
    [XD_UPDATE_ACTION_INSTALL] = "install",
    [XD_UPDATE_ACTION_DOWNLOAD] = "download",
    [XD_UPDATE_ACTION_CHECK] = "check",
};
static const char *const result_keys[XD_UPDATE_RESULT_COUNT] = {
    [XD_UPDATE_SUCCEEDED] = "succeeded",
    [XD_UPDATE_FAILED] = "failed",
    [XD_UPDATE_CANCELED] = "canceled",
};
/* A backend's details can list thousands of files; the history keeps the
 * start of them. */
static constexpr size_t MAX_DETAILS = 64 * 1024;

char *xd_update_history_path(void) {
    xd_strbuf path = {0};
    xd_state_path("update-history.ini", &path);
    char *out = g_strdup(xd_strbuf_cstr(&path));
    xd_strbuf_free(&path);
    return out;
}

const char *xd_update_result_name(xd_update_result result) {
    static const char *const names[XD_UPDATE_RESULT_COUNT] = {
        [XD_UPDATE_SUCCEEDED] = "Succeeded",
        [XD_UPDATE_FAILED] = "Failed",
        [XD_UPDATE_CANCELED] = "Canceled",
    };
    return (unsigned)result < XD_UPDATE_RESULT_COUNT ? names[result] : "";
}

void xd_update_history_entry_free(xd_update_history_entry *entry) {
    if (entry == nullptr)
        return;
    g_strfreev(entry->updates);
    g_free(entry->error);
    g_free(entry->summary);
    g_free(entry->details);
    g_free(entry);
}

static int find_key(const char *const *keys, int count, const char *value) {
    for (int i = 0; value != nullptr && i < count; i++) {
        if (strcmp(keys[i], value) == 0)
            return i;
    }
    return -1;
}

static char *get_text(GKeyFile *kf, const char *group, const char *key) {
    char *value = g_key_file_get_string(kf, group, key, nullptr);
    return value != nullptr ? value : g_strdup("");
}

GPtrArray *xd_update_history_read(const char *path) {
    GPtrArray *out =
        g_ptr_array_new_with_free_func((GDestroyNotify)xd_update_history_entry_free);
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, nullptr)) {
        g_key_file_free(kf);
        return out;
    }
    /* The groups in the file's order, which is newest first. */
    char **groups = g_key_file_get_groups(kf, nullptr);
    for (char **group = groups; *group != nullptr && out->len < XD_UPDATE_HISTORY_MAX; group++) {
        char *action = g_key_file_get_string(kf, *group, "Action", nullptr);
        char *result = g_key_file_get_string(kf, *group, "Result", nullptr);
        int action_index = find_key(action_keys, XD_UPDATE_ACTION_COUNT, action);
        int result_index = find_key(result_keys, XD_UPDATE_RESULT_COUNT, result);
        g_free(action);
        g_free(result);
        if (action_index < 0 || result_index < 0)
            continue;
        xd_update_history_entry *entry = g_new0(xd_update_history_entry, 1);
        entry->time = XD_MAX(g_key_file_get_int64(kf, *group, "Time", nullptr), 0);
        entry->action = (xd_update_action)action_index;
        entry->result = (xd_update_result)result_index;
        entry->updates = g_key_file_get_string_list(kf, *group, "Updates", nullptr, nullptr);
        if (entry->updates == nullptr)
            entry->updates = g_new0(char *, 1);
        entry->error = get_text(kf, *group, "Error");
        entry->summary = get_text(kf, *group, "Summary");
        entry->details = get_text(kf, *group, "Details");
        g_ptr_array_add(out, entry);
    }
    g_strfreev(groups);
    g_key_file_free(kf);
    return out;
}

static void set_entry(GKeyFile *kf, const char *group, const xd_update_history_entry *entry) {
    g_key_file_set_int64(kf, group, "Time", entry->time);
    g_key_file_set_string(kf, group, "Action", action_keys[entry->action]);
    g_key_file_set_string(kf, group, "Result", result_keys[entry->result]);
    const char *const none[] = {nullptr};
    const char *const *updates =
        entry->updates != nullptr ? (const char *const *)entry->updates : none;
    gsize count = 0;
    while (updates[count] != nullptr)
        count++;
    g_key_file_set_string_list(kf, group, "Updates", updates, count);
    g_key_file_set_string(kf, group, "Error", entry->error != nullptr ? entry->error : "");
    g_key_file_set_string(kf, group, "Summary", entry->summary != nullptr ? entry->summary : "");
    const char *details = entry->details != nullptr ? entry->details : "";
    if (strlen(details) > MAX_DETAILS) {
        char *start = g_utf8_make_valid(details, (gssize)MAX_DETAILS);
        g_key_file_set_string(kf, group, "Details", start);
        g_free(start);
    } else {
        g_key_file_set_string(kf, group, "Details", details);
    }
}

bool xd_update_history_add(const char *path, const xd_update_history_entry *entry) {
    XD_ASSERT((unsigned)entry->action < XD_UPDATE_ACTION_COUNT &&
              (unsigned)entry->result < XD_UPDATE_RESULT_COUNT);
    GPtrArray *old = xd_update_history_read(path);
    GKeyFile *kf = g_key_file_new();
    set_entry(kf, "Entry 1", entry);
    for (guint i = 0; i < old->len && i + 1 < XD_UPDATE_HISTORY_MAX; i++) {
        char *group = g_strdup_printf("Entry %u", i + 2);
        set_entry(kf, group, g_ptr_array_index(old, i));
        g_free(group);
    }
    gsize len = 0;
    char *data = g_key_file_to_data(kf, &len, nullptr);
    bool ok = g_file_set_contents(path, data, (gssize)len, nullptr);
    g_free(data);
    g_key_file_free(kf);
    g_ptr_array_unref(old);
    return ok;
}
