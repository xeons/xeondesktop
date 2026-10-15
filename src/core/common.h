/* SPDX-License-Identifier: MIT
 * Common definitions shared by every XeonDesktop component.
 *
 * Including this header also poisons libc functions that cannot be used
 * safely (unbounded copies, unchecked formatting). Use the xd_str / xd_strbuf
 * helpers instead.
 */
#ifndef XD_CORE_COMMON_H
#define XD_CORE_COMMON_H

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#error "XeonDesktop requires C23"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdckdint.h>

#define XD_PI 3.14159265358979323846

#define XD_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define XD_MIN(a, b) ((a) < (b) ? (a) : (b))
#define XD_MAX(a, b) ((a) > (b) ? (a) : (b))
#define XD_CLAMP(v, lo, hi) XD_MIN(XD_MAX((v), (lo)), (hi))

#if defined(__GNUC__) || defined(__clang__)
#define XD_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#define XD_LIKELY(x) __builtin_expect(!!(x), 1)
#define XD_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define XD_PRINTF(fmt_idx, arg_idx)
#define XD_LIKELY(x) (x)
#define XD_UNLIKELY(x) (x)
#endif

/* Aborts with a message. Used for invariant violations, never for
 * recoverable errors. */
[[noreturn]] void xd_panic_at(const char *file, int line, const char *msg);

#define XD_PANIC(msg) xd_panic_at(__FILE__, __LINE__, (msg))
#define XD_ASSERT(cond)                                                        \
    do {                                                                       \
        if (XD_UNLIKELY(!(cond)))                                              \
            xd_panic_at(__FILE__, __LINE__, "assertion failed: " #cond);       \
    } while (0)

/* Ban unsafe libc functions project-wide. The system headers that declare
 * them must be included before this point, which <stdio.h>/<string.h> users
 * get by including those first. */
#include <stdio.h>
#include <string.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC poison strcpy strcat sprintf vsprintf gets strtok strncpy strncat
#endif

#endif
