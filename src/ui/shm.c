/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "shm.h"

static void buffer_release(void *data, [[maybe_unused]] struct wl_buffer *wl_buffer) {
    xd_shm_buffer *buffer = data;
    buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

static void buffer_destroy(xd_shm_buffer *buffer) {
    if (buffer->surface != nullptr)
        cairo_surface_destroy(buffer->surface);
    if (buffer->buffer != nullptr)
        wl_buffer_destroy(buffer->buffer);
    if (buffer->data != nullptr)
        munmap(buffer->data, buffer->size);
    *buffer = (xd_shm_buffer){0};
}

static bool buffer_create(xd_shm_buffer *buffer, struct wl_shm *shm, int width, int height) {
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
    buffer->buffer =
        wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    buffer->data = data;
    buffer->size = size;
    buffer->width = width;
    buffer->height = height;
    buffer->busy = false;
    buffer->surface =
        cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, width, height, stride);
    wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
    if (cairo_surface_status(buffer->surface) != CAIRO_STATUS_SUCCESS) {
        buffer_destroy(buffer);
        return false;
    }
    return true;
}

xd_shm_buffer *xd_shm_ring_acquire(xd_shm_ring *ring, struct wl_shm *shm, int width,
                                   int height) {
    for (size_t i = 0; i < XD_SHM_RING; i++) {
        xd_shm_buffer *buffer = &ring->slots[i];
        if (buffer->busy)
            continue;
        if (buffer->buffer != nullptr && (buffer->width != width || buffer->height != height))
            buffer_destroy(buffer);
        if (buffer->buffer == nullptr && !buffer_create(buffer, shm, width, height))
            return nullptr;
        return buffer;
    }
    return nullptr;
}

void xd_shm_ring_finish(xd_shm_ring *ring) {
    for (size_t i = 0; i < XD_SHM_RING; i++)
        buffer_destroy(&ring->slots[i]);
}
