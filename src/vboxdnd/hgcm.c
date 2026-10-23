/* SPDX-License-Identifier: MIT */
#include "hgcm.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/vboxguest.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DEVICE "/dev/vboxuser"

static constexpr size_t CALL_HEADER = sizeof(struct vbg_ioctl_hgcm_call);
static constexpr size_t PARM_SIZE = sizeof(struct vmmdev_hgcm_function_parameter64);
static constexpr uint32_t PARMS_MAX = 32;

static void init_header(struct vbg_ioctl_hdr *header, uint32_t size_in, uint32_t size_out) {
    *header = (struct vbg_ioctl_hdr){
        .size_in = size_in,
        .version = VBG_IOCTL_HDR_VERSION,
        .type = VBG_IOCTL_HDR_TYPE_DEFAULT,
        .rc = XD_VERR_GENERAL_FAILURE,
        .size_out = size_out,
    };
}

size_t xd_hgcm_encode(uint32_t client_id, uint32_t function, const xd_hgcm_parm *parms,
                      uint32_t count, void *out, size_t size) {
    size_t needed = CALL_HEADER + PARM_SIZE * count;
    if (count > PARMS_MAX || size < needed)
        return 0;
    memset(out, 0, needed);
    struct vbg_ioctl_hgcm_call call = {
        .client_id = client_id,
        .function = function,
        .timeout_ms = UINT32_MAX, /* the host decides when */
        .interruptible = 1,
        .parm_count = (uint16_t)count,
    };
    init_header(&call.hdr, (uint32_t)needed, (uint32_t)needed);
    memcpy(out, &call, sizeof(call));
    unsigned char *at = (unsigned char *)out + CALL_HEADER;
    for (uint32_t i = 0; i < count; i++, at += PARM_SIZE) {
        struct vmmdev_hgcm_function_parameter64 parm = {0};
        switch (parms[i].type) {
        case XD_HGCM_U32:
            parm.type = VMMDEV_HGCM_PARM_TYPE_32BIT;
            parm.u.value32 = parms[i].u32;
            break;
        case XD_HGCM_U64:
            parm.type = VMMDEV_HGCM_PARM_TYPE_64BIT;
            parm.u.value64 = parms[i].u64;
            break;
        case XD_HGCM_BUFFER:
        default:
            parm.type = VMMDEV_HGCM_PARM_TYPE_LINADDR;
            parm.u.pointer.size = parms[i].size;
            parm.u.pointer.u.linear_addr = (uint64_t)(uintptr_t)parms[i].buffer;
            break;
        }
        memcpy(at, &parm, sizeof(parm));
    }
    return needed;
}

void xd_hgcm_decode(const void *call, xd_hgcm_parm *parms, uint32_t count) {
    const unsigned char *at = (const unsigned char *)call + CALL_HEADER;
    for (uint32_t i = 0; i < count; i++, at += PARM_SIZE) {
        struct vmmdev_hgcm_function_parameter64 parm;
        memcpy(&parm, at, sizeof(parm));
        if (parms[i].type == XD_HGCM_U32)
            parms[i].u32 = parm.u.value32;
        else if (parms[i].type == XD_HGCM_U64)
            parms[i].u64 = parm.u.value64;
        /* Buffers are written in place; their sizes don't change. */
    }
}

/* ---- the driver ---------------------------------------------------------------------- */

typedef struct device {
    xd_hgcm base;
    int fd;
    uint32_t client_id;
    atomic_bool disconnected;
} device;

static int device_call(xd_hgcm *hgcm, uint32_t function, xd_hgcm_parm *parms, uint32_t count) {
    device *dev = (device *)hgcm;
    if (atomic_load(&dev->disconnected))
        return XD_VERR_CANCELLED;
    unsigned char request[CALL_HEADER + PARM_SIZE * PARMS_MAX];
    size_t size = xd_hgcm_encode(dev->client_id, function, parms, count, request, sizeof(request));
    if (size == 0)
        return XD_VERR_INVALID_PARAMETER;
    if (ioctl(dev->fd, VBG_IOCTL_HGCM_CALL(size), request) != 0)
        return errno == EINTR ? XD_VERR_INTERRUPTED : XD_VERR_GENERAL_FAILURE;
    xd_hgcm_decode(request, parms, count);
    struct vbg_ioctl_hdr header;
    memcpy(&header, request, sizeof(header));
    return header.rc;
}

static void device_disconnect(xd_hgcm *hgcm) {
    device *dev = (device *)hgcm;
    if (atomic_exchange(&dev->disconnected, true))
        return;
    struct vbg_ioctl_hgcm_disconnect request = {.u.in.client_id = dev->client_id};
    init_header(&request.hdr, sizeof(request), sizeof(struct vbg_ioctl_hdr));
    ioctl(dev->fd, VBG_IOCTL_HGCM_DISCONNECT, &request);
}

static void device_free(xd_hgcm *hgcm) {
    device *dev = (device *)hgcm;
    device_disconnect(hgcm);
    close(dev->fd);
    free(dev);
}

xd_hgcm *xd_hgcm_open(const char *service, int *rc) {
    int fd = open(DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        *rc = XD_VERR_NOT_FOUND;
        return nullptr;
    }
    struct vbg_ioctl_hgcm_connect request = {0};
    init_header(&request.hdr, sizeof(request), sizeof(struct vbg_ioctl_hdr) + sizeof(uint32_t));
    request.u.in.loc.type = VMMDEV_HGCM_LOC_LOCALHOST_EXISTING;
    strncpy(request.u.in.loc.u.localhost.service_name, service,
            sizeof(request.u.in.loc.u.localhost.service_name) - 1);
    if (ioctl(fd, VBG_IOCTL_HGCM_CONNECT, &request) != 0 || request.hdr.rc < 0 ||
        request.u.out.client_id == 0) {
        *rc = request.hdr.rc < 0 ? request.hdr.rc : XD_VERR_GENERAL_FAILURE;
        close(fd);
        return nullptr;
    }
    device *dev = calloc(1, sizeof(*dev));
    if (dev == nullptr) {
        close(fd);
        *rc = XD_VERR_GENERAL_FAILURE;
        return nullptr;
    }
    dev->base = (xd_hgcm){.call = device_call, .disconnect = device_disconnect, .free = device_free};
    dev->fd = fd;
    dev->client_id = request.u.out.client_id;
    *rc = XD_VINF_SUCCESS;
    return &dev->base;
}
