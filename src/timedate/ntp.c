/* SPDX-License-Identifier: MIT */
#include "ntp.h"

static void add_servers(GPtrArray *out, const char *const *list) {
    for (size_t i = 0; list != nullptr && list[i] != nullptr; i++) {
        if (list[i][0] == '\0')
            continue;
        bool seen = false;
        for (guint k = 0; k < out->len && !seen; k++)
            seen = g_ascii_strcasecmp(out->pdata[k], list[i]) == 0;
        if (!seen)
            g_ptr_array_add(out, g_strdup(list[i]));
    }
}

char **xd_ntp_server_list(const char *const *runtime, const char *const *link,
                          const char *const *system, const char *const *fallback) {
    GPtrArray *out = g_ptr_array_new();
    add_servers(out, runtime);
    add_servers(out, system);
    add_servers(out, link);
    add_servers(out, fallback);
    g_ptr_array_add(out, nullptr);
    return (char **)g_ptr_array_free(out, FALSE);
}

gint64 xd_ntp_message_received(GVariant *message) {
    /* Leap, Version, Mode, Stratum, Precision, RootDelay, RootDispersion,
     * Reference, OriginateTimestamp, ReceiveTimestamp, TransmitTimestamp,
     * DestinationTimestamp, Ignored, PacketCount, Jitter. */
    if (message == nullptr ||
        !g_variant_is_of_type(message, G_VARIANT_TYPE("(uuuuittayttttbtt)")))
        return 0;
    guint64 dest = 0;
    g_variant_get_child(message, 11, "t", &dest);
    return dest <= G_MAXINT64 ? (gint64)dest : 0;
}

/* Windows' short time, "7:08 PM", or 24-hour where the locale has no
 * AM/PM (as the taskbar clock shows it). */
static char *short_time(GDateTime *when) {
    char *ampm = g_date_time_format(when, "%p");
    bool twelve = ampm != nullptr && ampm[0] != '\0';
    g_free(ampm);
    return g_date_time_format(when, twelve ? "%-I:%M %p" : "%H:%M");
}

char *xd_ntp_synced_text(const char *server, GDateTime *when) {
    char *date = g_date_time_format(when, "%x"), *time = short_time(when);
    char *text = g_strdup_printf(
        "The time has been successfully synchronized with %s on %s at %s.", server, date, time);
    g_free(time);
    g_free(date);
    return text;
}

char *xd_ntp_next_text(GDateTime *when) {
    char *date = g_date_time_format(when, "%x"), *time = short_time(when);
    char *text = g_strdup_printf("Next synchronization: %s at %s", date, time);
    g_free(time);
    g_free(date);
    return text;
}

char *xd_ntp_failed_text(const char *server, const char *reason) {
    if (reason == nullptr || reason[0] == '\0')
        return g_strdup_printf("An error occurred while synchronizing with %s.", server);
    return g_strdup_printf("An error occurred while synchronizing with %s.\n%s", server, reason);
}
