/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "../core/common.h"
#include "../settings/state.h"
#include "state.h"

static constexpr char GROUP[] = "Updates";

int64_t xd_updates_next_check(const xd_update_settings *u, int64_t last, GTimeZone *tz) {
    if (u->mode == XD_UPDATES_OFF)
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
    int want = u->day == 0 ? 0 : u->day == 1 ? 7 : u->day - 1;
    int64_t due = last + 24 * 3600;
    GDateTime *midnight = g_date_time_new(tz, g_date_time_get_year(at),
                                          g_date_time_get_month(at),
                                          g_date_time_get_day_of_month(at), 0, 0, 0);
    /* Today's time if it's still to come, else the next day (of the
     * week) with it; a week and a day covers every case. */
    for (int i = 0; midnight != nullptr && i <= 8; i++) {
        GDateTime *day = g_date_time_add_days(midnight, i);
        GDateTime *slot = g_date_time_new(tz, g_date_time_get_year(day),
                                          g_date_time_get_month(day),
                                          g_date_time_get_day_of_month(day), u->hour, 0, 0);
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
    int n = g_key_file_get_integer(kf, GROUP, key, nullptr);
    return XD_CLAMP(n, 0, 1000000);
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

static int compare_ids(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
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
