/* SPDX-License-Identifier: MIT
 * The sandbox xd-thumbnailer decodes in: bubblewrap with every namespace
 * unshared (no network, no other processes), nothing of the file system
 * but /usr read-only, the one input file read-only and a small tmpfs for
 * output; a seccomp filter (libseccomp) against the syscalls an escape
 * would need; and resource limits (memory, CPU time, file size).
 */
#ifndef XD_THUMBNAILER_SANDBOX_H
#define XD_THUMBNAILER_SANDBOX_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Where the input file appears inside the sandbox, and where an external
 * thumbnailer writes its PNG. */
#define XD_SANDBOX_IN "/in"
#define XD_SANDBOX_OUT "/out/thumb.png"

/* Where `input` appears inside the sandbox. Free with g_free. */
char *xd_sandbox_input_path(const char *input);
/* Runs this program inside the sandbox with `args` after it, `input`
 * readable at XD_SANDBOX_IN/<its name>. Its stdout is a pipe; its stderr
 * is ours when `verbose`, else discarded. */
GSubprocess *xd_sandbox_spawn(const char *input, const char *const *args, gboolean verbose,
                              GError **err);

G_END_DECLS

#endif
