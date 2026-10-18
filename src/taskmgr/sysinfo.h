/* SPDX-License-Identifier: MIT
 * Process and system statistics for the Task Manager, from procps-ng's
 * libproc2 (the library behind ps and top).
 */
#ifndef XD_TASKMGR_SYSINFO_H
#define XD_TASKMGR_SYSINFO_H

#include <glib.h>

typedef struct xd_proc {
    int pid, ppid;
    const char *name; /* borrowed: valid until the next sample */
    const char *user; /* borrowed */
    guint64 cpu_ticks;  /* user + system, whole lifetime */
    guint cpu_delta;    /* ticks since the previous sample */
    guint64 mem_kb;     /* resident */
    guint64 vm_kb;      /* virtual */
    guint64 faults;     /* major + minor page faults */
    int threads, nice;
    int handles;           /* open files; -1 when not collected */
    guint64 io_read, io_write; /* bytes; 0 when not collected or not ours */
    guint64 peak_kb;           /* lifetime peak resident (VmHWM); 0 when not collected */
} xd_proc;

typedef struct xd_sysstats {
    int ncpu;
    double cpu, cpu_kernel;    /* 0..1, all CPUs together */
    double *cpu_each, *kernel_each; /* ncpu values each */
    double idle;               /* 0..1 */
    guint64 idle_ticks;        /* since boot, all CPUs */
    guint64 total_delta;       /* ticks in the last interval, all CPUs */
    guint64 mem_total_kb, mem_avail_kb, cache_kb;
    guint64 commit_kb, commit_limit_kb;
    guint64 kernel_paged_kb, kernel_nonpaged_kb;
    guint64 swap_total_kb, swap_free_kb;
    guint processes, threads;
    guint64 handles; /* open files, system-wide */
} xd_sysstats;

typedef struct xd_sysinfo xd_sysinfo;

/* Extra per-process values cost more to read, so they're optional. */
xd_sysinfo *xd_sysinfo_new(void);
void xd_sysinfo_free(xd_sysinfo *si);
void xd_sysinfo_set_extras(xd_sysinfo *si, gboolean handles, gboolean io);
/* Peak Mem Usage: each process's VmHWM, read from /proc/PID/status. */
void xd_sysinfo_set_peak(xd_sysinfo *si, gboolean peak);

/* Takes a new sample. Returns FALSE if /proc couldn't be read. */
[[nodiscard]] gboolean xd_sysinfo_sample(xd_sysinfo *si);
const xd_proc *xd_sysinfo_procs(const xd_sysinfo *si, guint *count);
const xd_sysstats *xd_sysinfo_stats(const xd_sysinfo *si);
/* Clock ticks per second. */
long xd_sysinfo_hz(void);

#endif
