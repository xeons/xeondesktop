/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <seccomp.h>

#include "sandbox.h"

#if defined(__SANITIZE_ADDRESS__)
#define XD_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define XD_SANITIZED 1
#endif
#endif

/* Limits for one decode: a hostile file can make a decoder loop, allocate
 * without end or write without end, and must not take the machine with
 * it. Thumbnailing a video seeks and decodes a frame or two; nothing
 * legitimate comes near these. The file size limit is for the memfd glycin
 * hands a decoded image over in (a 6000x4000 photo is 96 MB of pixels);
 * files on disk are only /tmp and /out, tmpfs of 64 MB each. */
static constexpr rlim_t CPU_SECONDS = 20;
static constexpr rlim_t FILE_BYTES = 1024ull << 20;
static constexpr rlim_t OPEN_FILES = 256;
static const char TMPFS_BYTES[] = "67108864";

#define SET_LIMIT(resource, value)                                             \
    do {                                                                       \
        struct rlimit r_ = {(value), (value)};                                 \
        (void)setrlimit((resource), &r_);                                      \
    } while (0)

/* In the child, between fork and exec of bwrap (async-signal-safe only);
 * everything inside inherits the limits. */
static void limit_child([[maybe_unused]] gpointer data) {
    SET_LIMIT(RLIMIT_CPU, CPU_SECONDS);
    SET_LIMIT(RLIMIT_FSIZE, FILE_BYTES);
    SET_LIMIT(RLIMIT_NOFILE, OPEN_FILES);
    SET_LIMIT(RLIMIT_CORE, 0);
#ifndef XD_SANITIZED
    /* AddressSanitizer reserves terabytes of address space for its shadow
     * memory, so a sanitizer build can't run under an address-space
     * limit; release builds have it. */
    SET_LIMIT(RLIMIT_AS, 2048ull << 20);
#endif
}

/* Syscalls nothing that decodes a file needs, and that a compromised
 * decoder would use to get out or at the kernel: namespaces and mounts
 * (also refusing glycin's own nested bwrap, so it runs its loader directly
 * inside this sandbox), tracing other processes, kernel modules, BPF,
 * keyrings, io_uring, perf, userfaultfd, clock and system settings. */
static const char *const denied[] = {
    "unshare", "setns", "mount", "umount2", "pivot_root", "chroot", "mount_setattr",
    "move_mount", "open_tree", "fsopen", "fsconfig", "fsmount", "fspick", "ptrace",
    "process_vm_readv", "process_vm_writev", "pidfd_getfd", "kexec_load", "kexec_file_load",
    "init_module", "finit_module", "delete_module", "bpf", "perf_event_open", "userfaultfd",
    "keyctl", "add_key", "request_key", "io_uring_setup", "io_uring_enter",
    "io_uring_register", "open_by_handle_at", "name_to_handle_at", "fanotify_init", "syslog",
    "acct", "swapon", "swapoff", "reboot", "settimeofday", "clock_settime", "clock_adjtime",
    "adjtimex", "quotactl", "quotactl_fd", "sethostname", "setdomainname", "lookup_dcookie",
    "vhangup", "uselib", "ioperm", "iopl",
};

/* The filter as BPF in a memfd, for bwrap's --seccomp (loaded just before
 * it runs us, after its own setup). Other architectures' syscalls (32-bit
 * ones on x86_64) kill the process: libseccomp's default. */
static int seccomp_fd(GError **err) {
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
    if (ctx == nullptr) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "seccomp_init failed");
        return -1;
    }
    int rc = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(denied) && rc == 0; i++) {
        int nr = seccomp_syscall_resolve_name(denied[i]);
        if (nr != __NR_SCMP_ERROR)
            rc = seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), nr, 0);
    }
    /* clone3 passes its flags in memory a filter can't read: ENOSYS makes
     * the C library fall back to clone, whose flags it can. */
    if (rc == 0)
        rc = seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(clone3), 0);
    static const scmp_datum_t ns_flags[] = {CLONE_NEWUSER, CLONE_NEWNS,  CLONE_NEWNET,
                                            CLONE_NEWPID,  CLONE_NEWIPC, CLONE_NEWUTS,
                                            CLONE_NEWCGROUP};
    for (size_t i = 0; i < G_N_ELEMENTS(ns_flags) && rc == 0; i++)
        rc = seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(clone), 1,
                              SCMP_A0(SCMP_CMP_MASKED_EQ, ns_flags[i], ns_flags[i]));
    /* Local sockets only (glycin talks to its loader over a socket pair);
     * the network namespace is empty anyway. */
    if (rc == 0)
        rc = seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
                              SCMP_A0(SCMP_CMP_NE, AF_UNIX, 0));
    /* Pushing input into the terminal we were started from. */
    if (rc == 0)
        rc = seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(ioctl), 1,
                              SCMP_A1(SCMP_CMP_MASKED_EQ, 0xffffffffu, TIOCSTI));
    int fd = rc == 0 ? memfd_create("xd-thumbnailer-seccomp", MFD_CLOEXEC) : -1;
    if (fd >= 0 && (seccomp_export_bpf(ctx, fd) != 0 || lseek(fd, 0, SEEK_SET) != 0)) {
        close(fd);
        fd = -1;
    }
    seccomp_release(ctx);
    if (fd < 0)
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "Can't build the seccomp filter");
    return fd;
}

static void add(GPtrArray *argv, const char *arg) { g_ptr_array_add(argv, g_strdup(arg)); }

/* An option and its one or two values. */
static void opt(GPtrArray *argv, const char *option, const char *value, const char *second_value) {
    add(argv, option);
    add(argv, value);
    if (second_value != nullptr)
        add(argv, second_value);
}

/* The top-level folders that hold programs and libraries, as the host
 * has them: a symbolic link into /usr where /usr is merged, else the
 * folder read-only. */
static void add_root_dirs(GPtrArray *argv) {
    static const char *const dirs[] = {"/bin", "/sbin", "/lib", "/lib32", "/lib64", "/libx32"};
    for (size_t i = 0; i < G_N_ELEMENTS(dirs); i++) {
        if (g_file_test(dirs[i], G_FILE_TEST_IS_SYMLINK)) {
            char *target = g_file_read_link(dirs[i], nullptr);
            if (target != nullptr)
                opt(argv, "--symlink", target, dirs[i]);
            g_free(target);
        } else if (g_file_test(dirs[i], G_FILE_TEST_IS_DIR)) {
            opt(argv, "--ro-bind", dirs[i], dirs[i]);
        }
    }
}

/* Under its own name: some thumbnailers go by the extension. */
char *xd_sandbox_input_path(const char *input) {
    char *name = g_path_get_basename(input);
    char *inside = g_build_filename(XD_SANDBOX_IN, name, nullptr);
    g_free(name);
    return inside;
}

GSubprocess *xd_sandbox_spawn(const char *input, const char *const *args, gboolean verbose,
                              GError **err) {
    char *self = g_file_read_link("/proc/self/exe", err);
    if (self == nullptr)
        return nullptr;
    int filter = seccomp_fd(err);
    if (filter < 0) {
        g_free(self);
        return nullptr;
    }
    char *inside = xd_sandbox_input_path(input);

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    add(argv, "bwrap");
    /* No network, IPC, other processes or host name; a user namespace
     * of our own in which no further ones can be made. */
    add(argv, "--unshare-all");
    add(argv, "--unshare-user");
    add(argv, "--disable-userns");
    add(argv, "--die-with-parent");
    add(argv, "--new-session");
    opt(argv, "--cap-drop", "ALL", nullptr);
    opt(argv, "--ro-bind", "/usr", "/usr");
    add_root_dirs(argv);
    opt(argv, "--ro-bind-try", "/etc/ld.so.cache", "/etc/ld.so.cache");
    /* Fonts, for SVG's and office documents' text. */
    opt(argv, "--ro-bind-try", "/etc/fonts", "/etc/fonts");
    opt(argv, "--proc", "/proc", nullptr);
    opt(argv, "--dev", "/dev", nullptr);
    opt(argv, "--size", TMPFS_BYTES, nullptr);
    opt(argv, "--tmpfs", "/tmp", nullptr);
    opt(argv, "--size", TMPFS_BYTES, nullptr);
    opt(argv, "--tmpfs", "/out", nullptr);
    opt(argv, "--ro-bind", self, "/app/xd-thumbnailer");
    opt(argv, "--ro-bind", input, inside);
    add(argv, "--clearenv");
    opt(argv, "--setenv", "HOME", "/tmp");
    opt(argv, "--setenv", "TMPDIR", "/tmp");
    opt(argv, "--setenv", "XDG_RUNTIME_DIR", "/tmp");
    opt(argv, "--setenv", "PATH", "/usr/bin:/bin");
    opt(argv, "--setenv", "LANG", "C.UTF-8");
#ifdef XD_SANITIZED
    /* LeakSanitizer stops the world with ptrace, which the filter refuses. */
    opt(argv, "--setenv", "ASAN_OPTIONS", "detect_leaks=0");
#endif
    opt(argv, "--chdir", "/tmp", nullptr);
    opt(argv, "--seccomp", "3", nullptr);
    add(argv, "/app/xd-thumbnailer");
    for (const char *const *arg = args; *arg != nullptr; arg++)
        add(argv, *arg);
    g_ptr_array_add(argv, nullptr);

    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        verbose ? G_SUBPROCESS_FLAGS_STDOUT_PIPE
                : (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                     G_SUBPROCESS_FLAGS_STDERR_SILENCE));
    g_subprocess_launcher_take_fd(launcher, filter, 3);
    g_subprocess_launcher_set_child_setup(launcher, limit_child, nullptr, nullptr);
    GSubprocess *proc =
        g_subprocess_launcher_spawnv(launcher, (const char *const *)argv->pdata, err);
    g_object_unref(launcher);
    g_ptr_array_unref(argv);
    g_free(inside);
    g_free(self);
    return proc;
}
