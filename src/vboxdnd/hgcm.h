/* SPDX-License-Identifier: MIT
 * HGCM, the VirtualBox guest's calls to host services, through the guest
 * driver's /dev/vboxuser (the kernel's linux/vboxguest.h ioctls), and the
 * same calls answered by a scripted host for tests.
 *
 * A call is a function number and up to 32 parameters: 32-bit and 64-bit
 * values, and buffers the host reads and may write. Results come back in
 * place. Every call returns a VirtualBox status: 0 or more is success.
 */
#ifndef XD_VBOXDND_HGCM_H
#define XD_VBOXDND_HGCM_H

#include <stddef.h>
#include <stdint.h>

/* VirtualBox status codes this program uses (IPRT's numbers). */
enum {
    XD_VINF_SUCCESS = 0,
    XD_VERR_GENERAL_FAILURE = -1,
    XD_VERR_INVALID_PARAMETER = -2,
    XD_VERR_NOT_SUPPORTED = -37,
    XD_VERR_ACCESS_DENIED = -38,
    XD_VERR_INTERRUPTED = -39,
    XD_VERR_TIMEOUT = -40,
    XD_VERR_BUFFER_OVERFLOW = -41,
    XD_VERR_CANCELLED = -70,
    XD_VERR_NOT_FOUND = -78,
    XD_VERR_PATH_NOT_FOUND = -103,
    XD_VERR_DISK_FULL = -152,
    XD_VERR_HGCM_SERVICE_NOT_FOUND = -2900,
};

typedef enum xd_hgcm_type : uint8_t {
    XD_HGCM_U32 = 1,
    XD_HGCM_U64 = 2,
    XD_HGCM_BUFFER = 4, /* read and written by the host */
} xd_hgcm_type;

typedef struct xd_hgcm_parm {
    xd_hgcm_type type;
    uint32_t u32;
    uint64_t u64;
    void *buffer;
    uint32_t size; /* the buffer's */
} xd_hgcm_parm;

static inline xd_hgcm_parm xd_hgcm_u32(uint32_t value) {
    return (xd_hgcm_parm){.type = XD_HGCM_U32, .u32 = value};
}
static inline xd_hgcm_parm xd_hgcm_u64(uint64_t value) {
    return (xd_hgcm_parm){.type = XD_HGCM_U64, .u64 = value};
}
static inline xd_hgcm_parm xd_hgcm_buffer(void *buffer, uint32_t size) {
    return (xd_hgcm_parm){.type = XD_HGCM_BUFFER, .buffer = buffer, .size = size};
}

/* Where calls go: the real host, or a scripted one. `call` may be made
 * from two threads at once (one blocked waiting for the host). */
typedef struct xd_hgcm xd_hgcm;
struct xd_hgcm {
    int (*call)(xd_hgcm *hgcm, uint32_t function, xd_hgcm_parm *parms, uint32_t count);
    /* Wakes a call blocked waiting for the host, with VERR_CANCELLED, and
     * every later one: the connection is over. */
    void (*disconnect)(xd_hgcm *hgcm);
    void (*free)(xd_hgcm *hgcm);
};

/* Connects to a host service through /dev/vboxuser; nullptr (and *rc set)
 * when there's no device or no such service. */
xd_hgcm *xd_hgcm_open(const char *service, int *rc);

/* The driver's layout of a call: its 40-byte header and 16-byte
 * parameters, for `call` into `out` (`size` bytes; 40 + 16 * count needed),
 * and the results read back into `parms`. Apart for the unit tests. */
size_t xd_hgcm_encode(uint32_t client_id, uint32_t function, const xd_hgcm_parm *parms,
                      uint32_t count, void *out, size_t size);
void xd_hgcm_decode(const void *call, xd_hgcm_parm *parms, uint32_t count);

#endif
