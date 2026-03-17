#include "malloc_types.h"
#include <string.h>

void arena_init(arena_t *arena, uint32_t id) {
    memset(arena, 0, sizeof(arena_t));
    pthread_mutex_init(&arena->lock, NULL);
    arena->id = id;
    atomic_store(&arena->thread_count, 0);
    atomic_store(&arena->allocated, 0);
    atomic_store(&arena->freed, 0);
}

/*
 * Remove a page from its bin list.
 */
static void bin_remove_page(arena_t *arena, page_meta_t *page) {
    uint8_t bin = page->bin_idx;
    if (page->prev) page->prev->next = page->next;
    if (page->next) page->next->prev = page->prev;
    if (arena->bins[bin] == page) {
        arena->bins[bin] = page->next;
    }
    page->next = NULL;
    page->prev = NULL;
}

/*
 * Insert a page at the head of its bin list.
 */
static void bin_insert_page(arena_t *arena, page_meta_t *page) {
    uint8_t bin = page->bin_idx;
    page->prev = NULL;
    page->next = arena->bins[bin];
    if (arena->bins[bin]) {
        arena->bins[bin]->prev = page;
    }
    arena->bins[bin] = page;
}

/*
 * Allocate from arena (slow path).
 * This is called when the thread's TLD cache doesn't have a usable page.
 *
 * Strategy:
 * 1. Find a page with remaining bump space
 * 2. If none, allocate a new page
 *
 * We do NOT touch local_free here — that's exclusively the owner thread's domain.
 */
void *arena_alloc(arena_t *arena, size_t bin_idx) {
    uint8_t bin = (uint8_t)bin_idx;

    pthread_mutex_lock(&arena->lock);

    /* Try existing pages that still have bump space */
    page_meta_t *page = arena->bins[bin];
    while (page) {
        if (page->bump_offset < page->bump_end) {
            void *base = page_start(page);
            void *slot = (char *)base + page->bump_offset;
            page->bump_offset += page->slot_size;
            atomic_fetch_add_explicit(&page->used, 1, memory_order_relaxed);
            atomic_fetch_add(&arena->allocated, page->slot_size);
            pthread_mutex_unlock(&arena->lock);
            return slot;
        }
        page = page->next;
    }

    /* No page with bump space — allocate a new page */
    page_meta_t *new_page = NULL;
    segment_t *seg = arena->segments;
    while (seg) {
        new_page = segment_alloc_page(seg, bin);
        if (new_page) break;
        seg = seg->next;
    }

    /* Need a new segment */
    if (!new_page) {
        seg = segment_create(arena);
        if (!seg) {
            pthread_mutex_unlock(&arena->lock);
            return NULL;
        }
        seg->next = arena->segments;
        if (arena->segments) arena->segments->prev = seg;
        arena->segments = seg;
        arena->segment_count++;

        new_page = segment_alloc_page(seg, bin);
        if (!new_page) {
            pthread_mutex_unlock(&arena->lock);
            return NULL;
        }
    }

    /* Insert new page into bin list */
    bin_insert_page(arena, new_page);

    /* Bump-allocate from the fresh page */
    void *base = page_start(new_page);
    void *slot = (char *)base + new_page->bump_offset;
    new_page->bump_offset += new_page->slot_size;
    atomic_fetch_add_explicit(&new_page->used, 1, memory_order_relaxed);
    atomic_fetch_add(&arena->allocated, new_page->slot_size);

    pthread_mutex_unlock(&arena->lock);
    return slot;
}

/*
 * Handle page retirement when it becomes empty.
 * Called under the arena lock.
 */
void arena_free(arena_t *arena, void *ptr, page_meta_t *page) {
    (void)ptr;

    if (!page_is_empty(page)) return;

    bin_remove_page(arena, page);

    segment_t *seg = page->segment;
    segment_free_page(seg, page);
}

/*
 * Get the arena for the current thread.
 */
arena_t *arena_get(void) {
    tld_t *tld = tld_get();
    if (tld && tld->arena) return tld->arena;

    int count = atomic_load(&g_state.arena_count);
    if (count == 0) return NULL;

    int idx = atomic_fetch_add(&g_state.arena_next, 1) % count;
    arena_t *arena = &g_state.arenas[idx];
    atomic_fetch_add(&arena->thread_count, 1);

    if (tld) tld->arena = arena;
    return arena;
}
