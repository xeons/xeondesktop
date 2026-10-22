/* SPDX-License-Identifier: MIT
 * What the Task Manager's Networking tab and Peak Mem Usage read from the
 * kernel, apart from libproc2: network adapters from sysfs
 * (/sys/class/net), and a process's lifetime peak resident memory (VmHWM
 * in /proc/PID/status, which libproc2 doesn't offer). No GTK, so the unit
 * tests can use it.
 */
#ifndef XD_TASKMGR_NETSTAT_H
#define XD_TASKMGR_NETSTAT_H

#include <glib.h>

/* XP's adapter states (the MIB_IF_OPER_STATUS values its list showed). */
typedef enum xd_netif_state {
    XD_NETIF_OPERATIONAL,     /* up, with a link */
    XD_NETIF_CONNECTING,      /* waiting for authentication (dormant) */
    XD_NETIF_DISCONNECTED,    /* enabled, no cable or no link */
    XD_NETIF_NON_OPERATIONAL, /* disabled (administratively down) */
} xd_netif_state;

typedef struct xd_netif {
    char *name;
    guint64 rx_bytes, tx_bytes;
    guint64 speed_mbps; /* 0: unknown (Wi-Fi, virtual adapters) */
    xd_netif_state state;
} xd_netif;

/* Every adapter under `root` (normally "/sys/class/net") but loopback,
 * sorted by name: a GArray of xd_netif. Free with xd_netifs_free. */
GArray *xd_netifs_read(const char *root);
void xd_netifs_free(GArray *ifs);

/* operstate and the IFF_UP flag -> XP's state. */
xd_netif_state xd_netif_state_from(const char *operstate, gboolean admin_up);
const char *xd_netif_state_text(xd_netif_state state);
/* "10 Mbps", "100 Mbps", "1 Gbps", "2.5 Gbps"; "" when unknown. */
char *xd_netif_speed_text(guint64 mbps);
/* Share of the link used: bytes both ways over `seconds`, against the
 * link speed (0..1; 0 when the speed is unknown). */
double xd_netif_utilization(guint64 bytes, double seconds, guint64 speed_mbps);
/* XP's "0 %", "0.12 %", "1.5 %", "37 %". */
char *xd_netif_utilization_text(double share);

/* VmHWM from the text of /proc/PID/status, in KiB; 0 if absent (kernel
 * threads). */
guint64 xd_status_peak_kb(const char *status);
/* The same for a process; 0 if it's gone. */
guint64 xd_proc_peak_kb(int pid);

#endif
