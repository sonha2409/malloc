#include "malloc_types.h"

/*
 * Slab allocator: the hot-path entry points.
 *
 * Fast alloc path (~15 instructions):
 *   TLD → bin_page[bin] → local_free list pop → return
 *   No lock, no atomics for the common case.
 *
 * Slow alloc path: arena_alloc (takes arena lock, bump/collect remote/new page).
 *
 * Fast free path:
 *   Check owner_tid → local free list push → return
 *   No lock, no atomics.
 *
 * Slow free path: remote free (atomic Treiber stack push).
 */

/*
 * Core slab allocation with optional zero-tracking.
 * If zeroed is non-NULL, *zeroed is set to true when the slot came from
 * bump allocation (virgin mmap memory, guaranteed zero), false otherwise.
 */
static void *slab_alloc_inner(size_t size, bool *zeroed) {
    if (__builtin_expect(size == 0, 0)) size = 1;
    if (__builtin_expect(size > MEDIUM_MAX, 0)) {
        if (zeroed) *zeroed = false;
        return large_alloc(size);
    }

    uint8_t bin = size_to_bin(size);
    size_t slot_size = bin_to_size(bin);

    if (__builtin_expect(slot_size > PAGE_SIZE_ALLOC / 2, 0)) {
        if (zeroed) *zeroed = false;
        return large_alloc(size);
    }

    /* Fast path: try thread-local cached page */
    tld_t *tld = tld_get();
    if (__builtin_expect(tld != NULL, 1)) {
        page_meta_t *page = tld->bin_page[bin];
        if (__builtin_expect(page != NULL && page->state == PAGE_ACTIVE, 1)) {
            /* Try local free list (no lock needed — only this thread touches it) */
            if (__builtin_expect(page->local_free != NULL, 1)) {
                void *slot = page->local_free;
                page->local_free = *(void **)slot;
                page->local_free_count--;
                atomic_fetch_add_explicit(&page->used, 1, memory_order_relaxed);
                if (zeroed) *zeroed = false; /* free-list slot: may be dirty */
                return slot;
            }
            /* Try bump allocator */
            if (page->bump_offset < page->bump_end) {
                void *base = page_start(page);
                void *slot = (char *)base + page->bump_offset;
                page->bump_offset += page->slot_size;
                atomic_fetch_add_explicit(&page->used, 1, memory_order_relaxed);
                if (zeroed) *zeroed = true; /* virgin mmap memory */
                return slot;
            }
            /* Try collecting remote frees (needs to be done carefully) */
            page_collect_remote(page);
            if (page->local_free) {
                void *slot = page->local_free;
                page->local_free = *(void **)slot;
                page->local_free_count--;
                atomic_fetch_add_explicit(&page->used, 1, memory_order_relaxed);
                if (zeroed) *zeroed = false;
                return slot;
            }
            /* Page is full, clear cache */
            tld->bin_page[bin] = NULL;
        }
    }

    /* Slow path: go through arena (takes lock) */
    arena_t *arena = arena_get();
    if (__builtin_expect(!arena, 0)) return NULL;

    void *result = arena_alloc(arena, (size_t)bin, zeroed);

    /* Cache the page we allocated from, but only if we can claim ownership.
     * Use CAS: only set owner if currently unowned (UINT32_MAX). */
    if (result && tld) {
        page_meta_t *page = ptr_to_page(result);
        if (page) {
            uint32_t expected = UINT32_MAX;
            if (atomic_compare_exchange_strong_explicit(
                    &page->owner_tid, &expected, tld->thread_id,
                    memory_order_acq_rel, memory_order_acquire)) {
                /* We claimed ownership — cache this page */
                tld->bin_page[bin] = page;
            } else if (expected == tld->thread_id) {
                /* We already own it — cache it */
                tld->bin_page[bin] = page;
            }
            /* Otherwise another thread owns it; don't cache */
        }
    }

    return result;
}

void *slab_alloc(size_t size) {
    return slab_alloc_inner(size, NULL);
}

void *slab_alloc_zeroed(size_t size, bool *zeroed) {
    return slab_alloc_inner(size, zeroed);
}

void slab_free(void *ptr) {
    if (__builtin_expect(!ptr, 0)) return;

    segment_t *seg = ptr_to_segment(ptr);
    if (__builtin_expect(!seg, 0)) return;

    page_meta_t *page = ptr_to_page(ptr);
    if (__builtin_expect(!page || page->state == PAGE_UNUSED, 0)) return;

    /* Check if current thread is the page owner for fast local free */
    tld_t *tld = tld_get();
    if (__builtin_expect(tld != NULL, 1)) {
        uint32_t owner = atomic_load_explicit(&page->owner_tid, memory_order_relaxed);
        if (__builtin_expect(owner == tld->thread_id, 1)) {
            /* Fast path: local free (no atomics needed) */
            *(void **)ptr = page->local_free;
            page->local_free = ptr;
            page->local_free_count++;
            atomic_fetch_sub_explicit(&page->used, 1, memory_order_relaxed);
            return;
        }
    }

    /* Slow path: remote free (atomic Treiber stack push) */
    page_free_slot_remote(page, ptr);
}
