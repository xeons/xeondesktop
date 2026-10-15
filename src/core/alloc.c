/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "alloc.h"

void xd_panic_at(const char *file, int line, const char *msg) {
    fprintf(stderr, "xeondesktop: panic at %s:%d: %s\n", file, line, msg);
    fflush(stderr);
    abort();
}

void *xd_alloc(size_t size) {
    void *p = malloc(size == 0 ? 1 : size);
    if (p == nullptr)
        XD_PANIC("out of memory");
    return p;
}

void *xd_alloc_zeroed(size_t count, size_t size) {
    void *p = calloc(count == 0 ? 1 : count, size == 0 ? 1 : size);
    if (p == nullptr)
        XD_PANIC("out of memory");
    return p;
}

void *xd_alloc_array(size_t count, size_t size) {
    size_t total;
    if (ckd_mul(&total, count, size))
        XD_PANIC("allocation size overflow");
    return xd_alloc(total);
}

void *xd_realloc_array(void *ptr, size_t count, size_t size) {
    size_t total;
    if (ckd_mul(&total, count, size))
        XD_PANIC("allocation size overflow");
    void *p = realloc(ptr, total == 0 ? 1 : total);
    if (p == nullptr)
        XD_PANIC("out of memory");
    return p;
}

void xd_free(void *ptr) { free(ptr); }

struct xd_arena_block {
    xd_arena_block *next;
    size_t capacity;
    size_t used;
    alignas(max_align_t) unsigned char data[];
};

void xd_arena_init(xd_arena *arena, size_t block_size) {
    arena->head = nullptr;
    arena->block_size = block_size < 256 ? 256 : block_size;
}

void xd_arena_reset(xd_arena *arena) {
    xd_arena_destroy(arena);
}

void xd_arena_destroy(xd_arena *arena) {
    xd_arena_block *b = arena->head;
    while (b != nullptr) {
        xd_arena_block *next = b->next;
        xd_free(b);
        b = next;
    }
    arena->head = nullptr;
}

static bool is_pow2(size_t v) { return v != 0 && (v & (v - 1)) == 0; }

void *xd_arena_alloc(xd_arena *arena, size_t size, size_t align) {
    XD_ASSERT(is_pow2(align) && align <= alignof(max_align_t));

    xd_arena_block *b = arena->head;
    if (b != nullptr) {
        size_t start = (b->used + (align - 1)) & ~(align - 1);
        size_t end;
        if (start >= b->used && !ckd_add(&end, start, size) && end <= b->capacity) {
            b->used = end;
            return memset(b->data + start, 0, size);
        }
    }

    size_t capacity = XD_MAX(arena->block_size, size);
    size_t total;
    if (ckd_add(&total, sizeof(xd_arena_block), capacity))
        XD_PANIC("arena allocation size overflow");
    b = xd_alloc(total);
    b->next = arena->head;
    b->capacity = capacity;
    b->used = size;
    arena->head = b;
    return memset(b->data, 0, size);
}

void *xd_arena_alloc_array(xd_arena *arena, size_t count, size_t size, size_t align) {
    size_t total;
    if (ckd_mul(&total, count, size))
        XD_PANIC("arena allocation size overflow");
    return xd_arena_alloc(arena, total, align);
}
