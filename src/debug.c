#include "malloc_types.h"

#if MALLOC_DEBUG

#include <stdio.h>
#include <stdlib.h>

/*
 * Debug mode: per-page allocation bitmap for double-free detection
 * and heap integrity checking.
 *
 * Each page has a bitmap with 1 bit per slot:
 *   bit SET   = slot is currently allocated
 *   bit CLEAR = slot is free or virgin (never allocated)
 *
 * All bitmap operations use atomic byte-level ops for thread safety,
 * since remote frees can modify the bitmap concurrently.
 */

static inline uint16_t debug_slot_index(page_meta_t *page, void *ptr) {
    uintptr_t base = (uintptr_t)page_start(page);
    uintptr_t addr = (uintptr_t)ptr;
    if (addr < base) return UINT16_MAX;
    uintptr_t offset = addr - base;
    if (offset % page->slot_size != 0) return UINT16_MAX;
    uint16_t idx = (uint16_t)(offset / page->slot_size);
    return (idx < page->capacity) ? idx : UINT16_MAX;
}

void debug_mark_allocated(page_meta_t *page, void *ptr) {
    uint16_t idx = debug_slot_index(page, ptr);
    if (idx == UINT16_MAX) {
        fprintf(stderr, "MALLOC DEBUG: invalid alloc pointer %p (page %p, slot_size %u)\n",
                ptr, (void *)page, page->slot_size);
        abort();
    }
    uint8_t mask = (uint8_t)(1u << (idx % 8));
    uint8_t old = __atomic_fetch_or(&page->debug_bitmap[idx / 8], mask, __ATOMIC_RELAXED);
    if (old & mask) {
        fprintf(stderr, "MALLOC DEBUG: slot already marked allocated! ptr=%p slot=%u page=%p\n",
                ptr, idx, (void *)page);
        abort();
    }
}

void debug_check_and_mark_freed(page_meta_t *page, void *ptr) {
    uint16_t idx = debug_slot_index(page, ptr);
    if (idx == UINT16_MAX) {
        fprintf(stderr, "MALLOC DEBUG: invalid free pointer %p (misaligned or out of bounds, page %p)\n",
                ptr, (void *)page);
        abort();
    }
    uint8_t mask = (uint8_t)(1u << (idx % 8));
    uint8_t old = __atomic_fetch_and(&page->debug_bitmap[idx / 8], (uint8_t)~mask, __ATOMIC_RELAXED);
    if (!(old & mask)) {
        fprintf(stderr, "MALLOC DEBUG: double-free detected! ptr=%p slot=%u page=%p\n",
                ptr, idx, (void *)page);
        abort();
    }
}

/*
 * Full heap integrity check. Best called in a quiescent state
 * (single-threaded or after stopping all allocator activity).
 *
 * Validates per page:
 *   1. bitmap popcount == used count
 *   2. local_free entries have bitmap bits CLEAR and are valid
 *   3. local_free_count matches actual walk length
 *   4. used + local_free + remote_free + remaining_bump == capacity
 */
bool debug_heap_check(void) {
    int arena_count = atomic_load(&g_state.arena_count);
    bool ok = true;

    for (int a = 0; a < arena_count; a++) {
        arena_t *arena = &g_state.arenas[a];
        pthread_mutex_lock(&arena->lock);

        segment_t *seg = arena->segments;
        while (seg) {
            for (size_t p = PAGES_DATA_START; p < PAGES_PER_SEGMENT; p++) {
                page_meta_t *page = &seg->pages[p];
                if (page->state != PAGE_ACTIVE && page->state != PAGE_FULL)
                    continue;

                /* 1. Count set bits in bitmap */
                uint16_t bitmap_count = 0;
                uint16_t bitmap_bytes = (page->capacity + 7) / 8;
                for (uint16_t b = 0; b < bitmap_bytes; b++) {
                    uint8_t byte = __atomic_load_n(&page->debug_bitmap[b], __ATOMIC_RELAXED);
                    bitmap_count += (uint16_t)__builtin_popcount(byte);
                }

                uint16_t used = atomic_load(&page->used);
                if (bitmap_count != used) {
                    fprintf(stderr, "HEAP CHECK: page %p (seg %p idx %zu): "
                            "bitmap_count=%u != used=%u\n",
                            (void *)page, (void *)seg, p, bitmap_count, used);
                    ok = false;
                }

                /* 2. Walk local_free list — entries must have bitmap bit CLEAR */
                void *entry = page->local_free;
                uint16_t free_walk = 0;
                while (entry) {
                    free_walk++;
                    if (free_walk > page->capacity) {
                        fprintf(stderr, "HEAP CHECK: local_free cycle in page %p\n",
                                (void *)page);
                        ok = false;
                        break;
                    }
                    uint16_t idx = debug_slot_index(page, entry);
                    if (idx == UINT16_MAX) {
                        fprintf(stderr, "HEAP CHECK: invalid local_free entry %p in page %p\n",
                                entry, (void *)page);
                        ok = false;
                        break;
                    }
                    uint8_t byte = __atomic_load_n(&page->debug_bitmap[idx / 8], __ATOMIC_RELAXED);
                    if (byte & (1u << (idx % 8))) {
                        fprintf(stderr, "HEAP CHECK: local_free entry %p (slot %u) "
                                "has bitmap bit SET in page %p\n",
                                entry, idx, (void *)page);
                        ok = false;
                    }
                    entry = *(void **)entry;
                }

                /* 3. Verify local_free_count */
                if (free_walk != page->local_free_count) {
                    fprintf(stderr, "HEAP CHECK: page %p: local_free walk=%u "
                            "!= local_free_count=%u\n",
                            (void *)page, free_walk, page->local_free_count);
                    ok = false;
                }

                /* 4. Capacity invariant */
                uint16_t bump_remaining = 0;
                if (page->bump_end > page->bump_offset) {
                    bump_remaining = (uint16_t)((page->bump_end - page->bump_offset)
                                                / page->slot_size);
                }
                uint16_t remote_count = atomic_load(&page->remote_free_count);
                uint16_t accounted = used + free_walk + remote_count + bump_remaining;
                if (accounted != page->capacity) {
                    fprintf(stderr, "HEAP CHECK: page %p: used(%u) + local(%u) "
                            "+ remote(%u) + bump(%u) = %u != capacity(%u)\n",
                            (void *)page, used, free_walk, remote_count,
                            bump_remaining, accounted, page->capacity);
                    ok = false;
                }
            }
            seg = seg->next;
        }

        pthread_mutex_unlock(&arena->lock);
    }

    return ok;
}

#endif /* MALLOC_DEBUG */
