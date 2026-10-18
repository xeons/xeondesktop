/* SPDX-License-Identifier: MIT */
#include <net/if.h>
#include <string.h>

#include "netstat.h"

/* One small sysfs attribute, stripped; nullptr if unreadable. */
static char *read_attr(const char *dir, const char *name) {
    char *path = g_build_filename(dir, name, nullptr);
    char *text = nullptr;
    if (!g_file_get_contents(path, &text, nullptr, nullptr))
        text = nullptr;
    g_free(path);
    return text != nullptr ? g_strstrip(text) : nullptr;
}

static guint64 read_u64(const char *dir, const char *name, guint base) {
    char *text = read_attr(dir, name);
    guint64 v = 0;
    /* "flags" is "0x1003". */
    const char *digits = text;
    if (base == 16 && text != nullptr && g_str_has_prefix(text, "0x"))
        digits = text + 2;
    if (text != nullptr && !g_ascii_string_to_unsigned(digits, base, 0, G_MAXUINT64, &v, nullptr))
        v = 0;
    g_free(text);
    return v;
}

xd_netif_state xd_netif_state_from(const char *operstate, gboolean admin_up) {
    if (!admin_up)
        return XD_NETIF_NON_OPERATIONAL;
    if (operstate == nullptr)
        return XD_NETIF_DISCONNECTED;
    if (strcmp(operstate, "dormant") == 0 || strcmp(operstate, "testing") == 0)
        return XD_NETIF_CONNECTING;
    /* "unknown": drivers without link reporting (tun, many virtual
     * adapters), which pass traffic all the same. */
    if (strcmp(operstate, "up") == 0 || strcmp(operstate, "unknown") == 0)
        return XD_NETIF_OPERATIONAL;
    return XD_NETIF_DISCONNECTED;
}

const char *xd_netif_state_text(xd_netif_state s) {
    switch (s) {
    case XD_NETIF_OPERATIONAL:
        return "Operational";
    case XD_NETIF_CONNECTING:
        return "Connecting";
    case XD_NETIF_DISCONNECTED:
        return "Disconnected";
    case XD_NETIF_NON_OPERATIONAL:
        return "Non Operational";
    }
    return "";
}

static gint by_name(gconstpointer a, gconstpointer b) {
    return g_strcmp0(((const xd_netif *)a)->name, ((const xd_netif *)b)->name);
}

GArray *xd_netifs_read(const char *root) {
    GArray *ifs = g_array_new(FALSE, TRUE, sizeof(xd_netif));
    GDir *dir = g_dir_open(root, 0, nullptr);
    if (dir == nullptr)
        return ifs;
    const char *name;
    while ((name = g_dir_read_name(dir)) != nullptr) {
        char *d = g_build_filename(root, name, nullptr);
        /* Loopback (type 772, ARPHRD_LOOPBACK) isn't an adapter. */
        if (read_u64(d, "type", 10) == 772) {
            g_free(d);
            continue;
        }
        char *stats = g_build_filename(d, "statistics", nullptr);
        char *oper = read_attr(d, "operstate");
        gboolean up = (read_u64(d, "flags", 16) & IFF_UP) != 0;
        /* "speed" is -1 (which doesn't parse) or unreadable without a
         * link. */
        guint64 speed = read_u64(d, "speed", 10);
        xd_netif nif = {
            .name = g_strdup(name),
            .rx_bytes = read_u64(stats, "rx_bytes", 10),
            .tx_bytes = read_u64(stats, "tx_bytes", 10),
            .speed_mbps = speed < 10'000'000 ? speed : 0,
            .state = xd_netif_state_from(oper, up),
        };
        g_array_append_val(ifs, nif);
        g_free(oper);
        g_free(stats);
        g_free(d);
    }
    g_dir_close(dir);
    g_array_sort(ifs, by_name);
    return ifs;
}

void xd_netifs_free(GArray *ifs) {
    if (ifs == nullptr)
        return;
    for (guint i = 0; i < ifs->len; i++)
        g_free(g_array_index(ifs, xd_netif, i).name);
    g_array_unref(ifs);
}

char *xd_netif_speed_text(guint64 mbps) {
    if (mbps == 0)
        return g_strdup("");
    if (mbps < 1000)
        return g_strdup_printf("%" G_GUINT64_FORMAT " Mbps", mbps);
    if (mbps % 1000 == 0)
        return g_strdup_printf("%" G_GUINT64_FORMAT " Gbps", mbps / 1000);
    return g_strdup_printf("%.1f Gbps", (double)mbps / 1000.0);
}

double xd_netif_utilization(guint64 bytes, double seconds, guint64 speed_mbps) {
    if (speed_mbps == 0 || seconds <= 0)
        return 0;
    double share = (double)bytes * 8.0 / (seconds * (double)speed_mbps * 1e6);
    return CLAMP(share, 0.0, 1.0);
}

char *xd_netif_utilization_text(double share) {
    double pct = share * 100.0;
    if (pct < 0.005)
        return g_strdup("0 %");
    if (pct < 1)
        return g_strdup_printf("%.2f %%", pct);
    if (pct < 10)
        return g_strdup_printf("%.1f %%", pct);
    return g_strdup_printf("%.0f %%", pct);
}

guint64 xd_status_peak_kb(const char *status) {
    for (const char *line = status; line != nullptr && *line != '\0';) {
        if (g_str_has_prefix(line, "VmHWM:"))
            return g_ascii_strtoull(line + strlen("VmHWM:"), nullptr, 10);
        line = strchr(line, '\n');
        if (line != nullptr)
            line++;
    }
    return 0;
}

guint64 xd_proc_peak_kb(int pid) {
    char path[64];
    g_snprintf(path, sizeof(path), "/proc/%d/status", pid);
    char *text = nullptr;
    guint64 kb = 0;
    if (g_file_get_contents(path, &text, nullptr, nullptr))
        kb = xd_status_peak_kb(text);
    g_free(text);
    return kb;
}
