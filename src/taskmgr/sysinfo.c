/* SPDX-License-Identifier: MIT */
#include <libproc2/meminfo.h>
#include <libproc2/pids.h>
#include <libproc2/stat.h>
#include <stdio.h>

/* libproc2 4.0.4 (Ubuntu 26.04) takes the info as well; later versions
 * don't. */
#ifdef XD_PROC2_VAL_TAKES_INFO
#define XD_PIDS_VAL(item, type, stack) PIDS_VAL(item, type, stack, si->pids)
#define XD_STAT_VAL(item, type, stack) STAT_VAL(item, type, stack, si->stat)
#else
#define XD_PIDS_VAL(item, type, stack) PIDS_VAL(item, type, stack)
#define XD_STAT_VAL(item, type, stack) STAT_VAL(item, type, stack)
#endif
#include <unistd.h>

#include "netstat.h"
#include "sysinfo.h"

/* Process items, in stack order; the optional ones come last. */
enum {
    P_PID,
    P_PPID,
    P_CMD,
    P_USER,
    P_TICS,
    P_TICS_DELTA,
    P_RES,
    P_VIRT,
    P_FLT_MAJ,
    P_FLT_MIN,
    P_NLWP,
    P_NICE,
    P_BASE_COUNT,
};
static const enum pids_item base_items[P_BASE_COUNT] = {
    PIDS_ID_PID,  PIDS_ID_PPID,       PIDS_CMD,    PIDS_ID_EUSER, PIDS_TICS_ALL, PIDS_TICS_ALL_DELTA,
    PIDS_MEM_RES, PIDS_MEM_VIRT,      PIDS_FLT_MAJ, PIDS_FLT_MIN, PIDS_NLWP,     PIDS_NICE,
};

enum { S_ID, S_TOTAL, S_BUSY, S_SYSTEM, S_IDLE, S_COUNT };
static enum stat_item stat_items[S_COUNT] = {
    STAT_TIC_ID, STAT_TIC_SUM_DELTA_TOTAL, STAT_TIC_SUM_DELTA_BUSY, STAT_TIC_SUM_DELTA_SYSTEM,
    STAT_TIC_SUM_IDLE,
};

struct xd_sysinfo {
    struct pids_info *pids;
    struct stat_info *stat;
    struct meminfo_info *mem;
    enum pids_item items[P_BASE_COUNT + 3];
    int nitems;
    int i_handles, i_io_read, i_io_write; /* -1 when not collected */
    gboolean peak;                        /* read VmHWM */
    gboolean count_files;                 /* open files, without PIDS_OPEN_FILES */
    GArray *procs;                        /* xd_proc */
    xd_sysstats stats;
};

long xd_sysinfo_hz(void) {
    long hz = sysconf(_SC_CLK_TCK);
    return hz > 0 ? hz : 100;
}

static void set_items(xd_sysinfo *si, gboolean handles, gboolean io) {
    si->nitems = 0;
    for (int i = 0; i < P_BASE_COUNT; i++)
        si->items[si->nitems++] = base_items[i];
    si->i_handles = si->i_io_read = si->i_io_write = -1;
    si->count_files = FALSE;
    if (handles) {
#ifdef XD_PROC2_HAS_OPEN_FILES
        si->i_handles = si->nitems;
        si->items[si->nitems++] = PIDS_OPEN_FILES;
#else
        si->count_files = TRUE;
#endif
    }
    if (io) {
        si->i_io_read = si->nitems;
        si->items[si->nitems++] = PIDS_IO_READ_BYTES;
        si->i_io_write = si->nitems;
        si->items[si->nitems++] = PIDS_IO_WRITE_BYTES;
    }
}

xd_sysinfo *xd_sysinfo_new(void) {
    xd_sysinfo *si = g_new0(xd_sysinfo, 1);
    set_items(si, FALSE, FALSE);
    if (procps_pids_new(&si->pids, si->items, si->nitems) < 0 || procps_stat_new(&si->stat) < 0 ||
        procps_meminfo_new(&si->mem) < 0) {
        xd_sysinfo_free(si);
        return nullptr;
    }
    si->procs = g_array_new(FALSE, TRUE, sizeof(xd_proc));
    return si;
}

void xd_sysinfo_free(xd_sysinfo *si) {
    if (si == nullptr)
        return;
    if (si->pids != nullptr)
        procps_pids_unref(&si->pids);
    if (si->stat != nullptr)
        procps_stat_unref(&si->stat);
    if (si->mem != nullptr)
        procps_meminfo_unref(&si->mem);
    if (si->procs != nullptr)
        g_array_unref(si->procs);
    g_free(si->stats.cpu_each);
    g_free(si->stats.kernel_each);
    g_free(si);
}

void xd_sysinfo_set_extras(xd_sysinfo *si, gboolean handles, gboolean io) {
    if ((si->i_handles >= 0) == handles && (si->i_io_read >= 0) == io)
        return;
    set_items(si, handles, io);
    if (procps_pids_reset(si->pids, si->items, si->nitems) < 0)
        set_items(si, FALSE, FALSE);
}

void xd_sysinfo_set_peak(xd_sysinfo *si, gboolean peak) { si->peak = peak; }

static guint64 mem_value(xd_sysinfo *si, enum meminfo_item item) {
    struct meminfo_result *meminfo = procps_meminfo_get(si->mem, item);
    return meminfo != nullptr ? meminfo->result.ul_int : 0;
}

static double ratio(long part, long whole) {
    if (whole <= 0 || part <= 0)
        return 0;
    double fraction = (double)part / (double)whole;
    return fraction > 1 ? 1 : fraction;
}

static void sample_cpu(xd_sysinfo *si) {
    xd_sysstats *st = &si->stats;
    struct stat_reaped *reaped =
        procps_stat_reap(si->stat, STAT_REAP_CPUS_ONLY, stat_items, S_COUNT);
    if (reaped == nullptr)
        return;
    struct stat_stack *sum = reaped->summary;
    long total = XD_STAT_VAL(S_TOTAL, sl_int, sum);
    st->total_delta = total > 0 ? (guint64)total : 0;
    st->cpu = ratio(XD_STAT_VAL(S_BUSY, sl_int, sum), total);
    st->cpu_kernel = ratio(XD_STAT_VAL(S_SYSTEM, sl_int, sum), total);
    st->idle = total > 0 ? 1.0 - st->cpu : 1.0;
    st->idle_ticks = XD_STAT_VAL(S_IDLE, ull_int, sum);

    int cpu_count = reaped->cpus != nullptr ? reaped->cpus->total : 0;
    if (cpu_count != st->ncpu) {
        g_free(st->cpu_each);
        g_free(st->kernel_each);
        st->ncpu = cpu_count;
        st->cpu_each = g_new0(double, (gsize)MAX(cpu_count, 1));
        st->kernel_each = g_new0(double, (gsize)MAX(cpu_count, 1));
    }
    for (int i = 0; i < cpu_count; i++) {
        struct stat_stack *cpu_stack = reaped->cpus->stacks[i];
        int id = XD_STAT_VAL(S_ID, s_int, cpu_stack);
        if (id < 0 || id >= cpu_count)
            continue;
        long cpu_total = XD_STAT_VAL(S_TOTAL, sl_int, cpu_stack);
        st->cpu_each[id] = ratio(XD_STAT_VAL(S_BUSY, sl_int, cpu_stack), cpu_total);
        st->kernel_each[id] = ratio(XD_STAT_VAL(S_SYSTEM, sl_int, cpu_stack), cpu_total);
    }
}

static void sample_memory(xd_sysinfo *si) {
    xd_sysstats *st = &si->stats;
    st->mem_total_kb = mem_value(si, MEMINFO_MEM_TOTAL);
    st->mem_avail_kb = mem_value(si, MEMINFO_MEM_AVAILABLE);
    st->cache_kb = mem_value(si, MEMINFO_MEM_CACHED) + mem_value(si, MEMINFO_MEM_BUFFERS);
    st->commit_kb = mem_value(si, MEMINFO_MEM_COMMITTED_AS);
    st->commit_limit_kb = mem_value(si, MEMINFO_MEM_COMMIT_LIMIT);
    /* Reclaimable slab can be dropped under pressure, like Windows' paged
     * pool; the rest stays resident. */
    st->kernel_paged_kb = mem_value(si, MEMINFO_MEM_SLAB_RECLAIM);
    st->kernel_nonpaged_kb = mem_value(si, MEMINFO_MEM_SLAB_UNRECLAIM) +
                             mem_value(si, MEMINFO_MEM_KERNEL_STACK) +
                             mem_value(si, MEMINFO_MEM_PAGE_TABLES);
    st->swap_total_kb = mem_value(si, MEMINFO_SWAP_TOTAL);
    st->swap_free_kb = mem_value(si, MEMINFO_SWAP_FREE);

    /* Open files system-wide: the first number in file-nr. */
    char *text = nullptr;
    st->handles = 0;
    if (g_file_get_contents("/proc/sys/fs/file-nr", &text, nullptr, nullptr))
        st->handles = g_ascii_strtoull(text, nullptr, 10);
    g_free(text);
}

#ifndef XD_PROC2_HAS_OPEN_FILES
/* Older libproc2 (4.0.4) has no PIDS_OPEN_FILES: count /proc/PID/fd as it
 * does. -1 when it can't be read (other users' processes). */
static int open_files(int pid) {
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/fd", pid);
    GDir *dir = g_dir_open(path, 0, nullptr);
    if (dir == nullptr)
        return -1;
    int n = 0;
    while (g_dir_read_name(dir) != nullptr)
        n++;
    g_dir_close(dir);
    return n;
}
#endif

gboolean xd_sysinfo_sample(xd_sysinfo *si) {
    sample_cpu(si);
    sample_memory(si);

    g_array_set_size(si->procs, 0);
    struct pids_fetch *fetch_result = procps_pids_reap(si->pids, PIDS_FETCH_TASKS_ONLY);
    if (fetch_result == nullptr)
        return FALSE;
    int task_count = fetch_result->counts->total;
    guint threads = 0;
    for (int i = 0; i < task_count; i++) {
        struct pids_stack *process_stack = fetch_result->stacks[i];
        xd_proc proc = {
            .pid = XD_PIDS_VAL(P_PID, s_int, process_stack),
            .ppid = XD_PIDS_VAL(P_PPID, s_int, process_stack),
            .name = XD_PIDS_VAL(P_CMD, str, process_stack),
            .user = XD_PIDS_VAL(P_USER, str, process_stack),
            .cpu_ticks = XD_PIDS_VAL(P_TICS, ull_int, process_stack),
            .cpu_delta = XD_PIDS_VAL(P_TICS_DELTA, u_int, process_stack),
            .mem_kb = XD_PIDS_VAL(P_RES, ul_int, process_stack),
            .vm_kb = XD_PIDS_VAL(P_VIRT, ul_int, process_stack),
            .faults = (guint64)XD_PIDS_VAL(P_FLT_MAJ, ul_int, process_stack) +
                      XD_PIDS_VAL(P_FLT_MIN, ul_int, process_stack),
            .threads = XD_PIDS_VAL(P_NLWP, s_int, process_stack),
            .nice = XD_PIDS_VAL(P_NICE, s_int, process_stack),
            .handles = si->i_handles >= 0 ? XD_PIDS_VAL(si->i_handles, s_int, process_stack) : -1,
            .io_read = si->i_io_read >= 0 ? XD_PIDS_VAL(si->i_io_read, ul_int, process_stack) : 0,
            .io_write =
                si->i_io_write >= 0 ? XD_PIDS_VAL(si->i_io_write, ul_int, process_stack) : 0,
        };
        /* libproc2 has no item for VmHWM; one more read per process. */
        if (si->peak)
            proc.peak_kb = xd_proc_peak_kb(proc.pid);
#ifndef XD_PROC2_HAS_OPEN_FILES
        if (si->count_files)
            proc.handles = open_files(proc.pid);
#endif
        threads += (guint)MAX(proc.threads, 0);
        g_array_append_val(si->procs, proc);
    }
    si->stats.processes = (guint)MAX(task_count, 0);
    si->stats.threads = threads;
    return TRUE;
}

const xd_proc *xd_sysinfo_procs(const xd_sysinfo *si, guint *count) {
    *count = si->procs->len;
    return (const xd_proc *)(const void *)si->procs->data;
}

const xd_sysstats *xd_sysinfo_stats(const xd_sysinfo *si) { return &si->stats; }
