/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "shm.h"

static void buffer_release(void *data, [[maybe_unused]] struct wl_buffer *wl_buffer) {
    xd_shm_buffer *b = data;
    b->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

static void buffer_destroy(xd_shm_buffer *b) {
    if (b->surface != nullptr)
        cairo_surface_destroy(b->surface);
    if (b->buffer != nullptr)
        wl_buffer_destroy(b->buffer);
    if (b->data != nullptr)
        munmap(b->data, b->size);
    *b = (xd_shm_buffer){0};
}

static bool buffer_create(xd_shm_buffer *b, struct wl_shm *shm, int width, int height) {
    XD_ASSERT(width > 0 && height > 0);
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    size_t size;
    if (stride <= 0 || ckd_mul(&size, (size_t)stride, (size_t)height) || size > INT32_MAX)
        return false;

    int fd = memfd_create("xd-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return false;
    int rc;
    do {
        rc = ftruncate(fd, (off_t)size);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        close(fd);
        return false;
    }
    /* The compositor can't shrink the file under us. */
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);

    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    b->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                          WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    b->data = data;
    b->size = size;
    b->width = width;
    b->height = height;
    b->busy = false;
    b->surface = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, width, height,
                                                     stride);
    wl_buffer_add_listener(b->buffer, &buffer_listener, b);
    if (cairo_surface_status(b->surface) != CAIRO_STATUS_SUCCESS) {
        buffer_destroy(b);
        return false;
    }
    return true;
}

xd_shm_buffer *xd_shm_ring_acquire(xd_shm_ring *ring, struct wl_shm *shm, int width,
                                   int height) {
    for (size_t i = 0; i < XD_SHM_RING; i++) {
        xd_shm_buffer *b = &ring->slots[i];
        if (b->busy)
            continue;
        if (b->buffer != nullptr && (b->width != width || b->height != height))
            buffer_destroy(b);
        if (b->buffer == nullptr && !buffer_create(b, shm, width, height))
            return nullptr;
        return b;
    }
    return nullptr;
}

void xd_shm_ring_finish(xd_shm_ring *ring) {
    for (size_t i = 0; i < XD_SHM_RING; i++)
        buffer_destroy(&ring->slots[i]);
}
