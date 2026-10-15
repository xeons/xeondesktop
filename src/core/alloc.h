/* SPDX-License-Identifier: MIT
 * Checked allocation and arena allocator.
 *
 * All size computations go through ckd_* so an overflowing request fails
 * instead of returning an undersized block. Allocation failure is fatal:
 * a desktop shell cannot meaningfully continue without memory, and making
 * it fatal removes a whole class of unchecked-NULL bugs.
 */
#ifndef XD_CORE_ALLOC_H
#define XD_CORE_ALLOC_H

#include "common.h"

[[nodiscard]] void *xd_alloc(size_t size);
[[nodiscard]] void *xd_alloc_zeroed(size_t count, size_t size);
[[nodiscard]] void *xd_alloc_array(size_t count, size_t size);
[[nodiscard]] void *xd_realloc_array(void *ptr, size_t count, size_t size);
void xd_free(void *ptr);

/* Allocate one zeroed T / n zeroed Ts. */
#define XD_NEW(T) ((T *)xd_alloc_zeroed(1, sizeof(T)))
#define XD_NEW_ARRAY(T, n) ((T *)xd_alloc_zeroed((n), sizeof(T)))

/* Frees *pp and sets it to nullptr so it cannot be used again. */
#define XD_FREE(pp)                                                            \
    do {                                                                       \
        xd_free(*(pp));                                                        \
        *(pp) = nullptr;                                                       \
    } while (0)

/* Bump allocator. Everything allocated from an arena is released together
 * with xd_arena_reset() or xd_arena_destroy(). */
typedef struct xd_arena_block xd_arena_block;

typedef struct xd_arena {
    xd_arena_block *head;
    size_t block_size;
} xd_arena;

void xd_arena_init(xd_arena *arena, size_t block_size);
void xd_arena_reset(xd_arena *arena);
void xd_arena_destroy(xd_arena *arena);
[[nodiscard]] void *xd_arena_alloc(xd_arena *arena, size_t size, size_t align);
[[nodiscard]] void *xd_arena_alloc_array(xd_arena *arena, size_t count, size_t size, size_t align);

#define XD_ARENA_NEW(arena, T) ((T *)xd_arena_alloc((arena), sizeof(T), alignof(T)))
#define XD_ARENA_ARRAY(arena, T, n)                                            \
    ((T *)xd_arena_alloc_array((arena), (n), sizeof(T), alignof(T)))

#endif
