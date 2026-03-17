#include "malloc_types.h"
#include <string.h>
#include <stdio.h>

/*
 * Page-level allocation: bump allocator + free list.
 * Each page is a slab of fixed-size slots.
 */

void page_init(page_meta_t *page, segment_t *seg, uint16_t page_index,
               uint8_t bin_idx, uint32_t slot_size) {
    page->segment = seg;
    page->arena = seg->arena;
    page->page_index = page_index;
    page->bin_idx = bin_idx;
    page->slot_size = slot_size;
    page->state = PAGE_ACTIVE;

    /* Ensure slot_size is at least MIN_ALIGN and aligned */
    if (page->slot_size < MIN_ALIGN) page->slot_size = MIN_ALIGN;
    page->slot_size = (page->slot_size + (MIN_ALIGN - 1)) & ~(MIN_ALIGN - 1);

    page->capacity = (uint16_t)(PAGE_SIZE_ALLOC / page->slot_size);
    atomic_store(&page->used, 0);

    page->bump_offset = 0;
    page->bump_end = (uint32_t)(page->capacity * page->slot_size);

    page->local_free = NULL;
    page->local_free_count = 0;
    atomic_store(&page->remote_free.value, 0);
    atomic_store(&page->remote_free_count, 0);

    page->next = NULL;
    page->prev = NULL;
    atomic_store(&page->owner_tid, UINT32_MAX); /* no owner yet */

#if MALLOC_DEBUG
    memset(page->debug_bitmap, 0, DEBUG_BITMAP_BYTES);
#endif
}

/*
 * Allocate a slot from this page.
 * Fast path: bump allocator (for virgin pages).
 * Slow path: local free list, then collect remote frees.
 */
void *page_alloc_slot(page_meta_t *page) {
    /* Try local free list first (most likely after some frees) */
    if (page->local_free) {
        void *slot = page->local_free;
        page->local_free = *(void **)slot;
        page->local_free_count--;
        atomic_fetch_add(&page->used, 1);
        return slot;
    }

    /* Try bump allocator (virgin slots) */
    if (page->bump_offset < page->bump_end) {
        void *base = page_start(page);
        void *slot = (char *)base + page->bump_offset;
        page->bump_offset += page->slot_size;
        atomic_fetch_add(&page->used, 1);
        return slot;
    }

    /* Collect remote frees */
    page_collect_remote(page);
    if (page->local_free) {
        void *slot = page->local_free;
        page->local_free = *(void **)slot;
        page->local_free_count--;
        atomic_fetch_add(&page->used, 1);
        return slot;
    }

    return NULL; /* page is truly full */
}

/* Free a slot on the owning thread (local path) */
void page_free_slot_local(page_meta_t *page, void *ptr) {
    *(void **)ptr = page->local_free;
    page->local_free = ptr;
    page->local_free_count++;
    atomic_fetch_sub(&page->used, 1);
}

/* Free a slot from a non-owning thread (remote path, lock-free) */
void page_free_slot_remote(page_meta_t *page, void *ptr) {
    treiber_push(&page->remote_free, ptr, 0); /* next ptr is at offset 0 */
    atomic_fetch_add(&page->remote_free_count, 1);
    atomic_fetch_sub(&page->used, 1);
}

/* Collect remote frees into the local free list */
void page_collect_remote(page_meta_t *page) {
    void *list = treiber_collect(&page->remote_free);
    if (!list) return;

    /* Walk the collected list and prepend to local free list */
    uint16_t count = 0;
    void *current = list;
    void *tail = NULL;
    while (current) {
        tail = current;
        count++;
        void *next = *(void **)current;
        if (!next) break;
        current = next;
    }

    /* Append local_free to end of collected list */
    if (tail) {
        *(void **)tail = page->local_free;
    }
    page->local_free = list;
    page->local_free_count += count;
    atomic_fetch_sub(&page->remote_free_count, count);
}

bool page_is_empty(page_meta_t *page) {
    return atomic_load(&page->used) == 0 &&
           page->bump_offset > 0; /* has been initialized */
}

void page_retire(page_meta_t *page) {
    page->state = PAGE_RETIRED;
    void *start = page_start(page);
    os_madvise_free(start, PAGE_SIZE_ALLOC);
}
