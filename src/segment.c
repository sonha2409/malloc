#include "malloc_types.h"
#include <string.h>

/*
 * Segment: a 4 MB aligned mmap region.
 * Page 0 (first 64 KB) holds the segment_t header + page_meta array.
 * Pages 1–63 are data pages for slab allocation.
 */

/* Segment registry: open-addressing hash set of active segment base addresses.
 * This avoids dereferencing arbitrary aligned pointers to check magic. */

static inline size_t seg_hash(uintptr_t addr) {
    return (addr >> SEGMENT_SHIFT) & (SEGMENT_REGISTRY_SIZE - 1);
}

static void segment_register(uintptr_t base) {
    size_t idx = seg_hash(base);
    for (size_t i = 0; i < SEGMENT_REGISTRY_SIZE; i++) {
        size_t slot = (idx + i) & (SEGMENT_REGISTRY_SIZE - 1);
        uintptr_t expected = 0;
        if (atomic_compare_exchange_strong(&g_state.segment_registry[slot],
                                           &expected, base)) {
            return;
        }
        if (expected == base) return; /* already registered */
    }
}

static void segment_unregister(uintptr_t base) {
    size_t idx = seg_hash(base);
    for (size_t i = 0; i < SEGMENT_REGISTRY_SIZE; i++) {
        size_t slot = (idx + i) & (SEGMENT_REGISTRY_SIZE - 1);
        uintptr_t val = atomic_load(&g_state.segment_registry[slot]);
        if (val == base) {
            atomic_store(&g_state.segment_registry[slot], (uintptr_t)0);
            return;
        }
        if (val == 0) return; /* not found */
    }
}

static bool segment_is_registered(uintptr_t base) {
    size_t idx = seg_hash(base);
    for (size_t i = 0; i < SEGMENT_REGISTRY_SIZE; i++) {
        size_t slot = (idx + i) & (SEGMENT_REGISTRY_SIZE - 1);
        uintptr_t val = atomic_load(&g_state.segment_registry[slot]);
        if (val == base) return true;
        if (val == 0) return false;
    }
    return false;
}

segment_t *segment_create(arena_t *arena) {
    void *mem = os_mmap_aligned(SEGMENT_SIZE, SEGMENT_SIZE);
    if (!mem) return NULL;

    segment_t *seg = (segment_t *)mem;
    memset(seg, 0, sizeof(segment_t));
    seg->magic = SEGMENT_MAGIC;
    seg->page_count = PAGES_PER_SEGMENT;
    seg->pages_used = 0;
    seg->arena = arena;
    seg->next = NULL;
    seg->prev = NULL;

    /* Initialize all page metadata as unused */
    for (size_t i = 0; i < PAGES_PER_SEGMENT; i++) {
        seg->pages[i].state = PAGE_UNUSED;
        seg->pages[i].segment = seg;
        seg->pages[i].arena = arena;
        seg->pages[i].page_index = (uint16_t)i;
        seg->pages[i].local_free = NULL;
        seg->pages[i].local_free_count = 0;
        seg->pages[i].remote_free.value = 0;
        atomic_store(&seg->pages[i].remote_free_count, 0);
        seg->pages[i].next = NULL;
        seg->pages[i].prev = NULL;
    }

    /* Page 0 is reserved for the header */
    seg->pages[0].state = PAGE_FULL;

    /* Register in global segment registry */
    segment_register((uintptr_t)mem);

    return seg;
}

void segment_destroy(segment_t *seg) {
    if (!seg) return;
    segment_unregister((uintptr_t)seg);
    os_munmap(seg, SEGMENT_SIZE);
}

page_meta_t *segment_alloc_page(segment_t *seg, uint8_t bin_idx) {
    for (size_t i = PAGES_DATA_START; i < PAGES_PER_SEGMENT; i++) {
        if (seg->pages[i].state == PAGE_UNUSED) {
            uint32_t slot_size = (uint32_t)bin_to_size(bin_idx);
            page_init(&seg->pages[i], seg, (uint16_t)i, bin_idx, slot_size);
            seg->pages_used++;
            return &seg->pages[i];
        }
    }
    return NULL; /* segment full */
}

void segment_free_page(segment_t *seg, page_meta_t *page) {
    /* Return page memory to OS */
    void *start = page_start(page);
    os_madvise_free(start, PAGE_SIZE_ALLOC);

    atomic_store(&page->owner_tid, UINT32_MAX); /* invalidate ownership */
    page->state = PAGE_UNUSED;
    page->local_free = NULL;
    page->local_free_count = 0;
    atomic_store(&page->remote_free.value, 0);
    atomic_store(&page->remote_free_count, 0);
    atomic_store(&page->used, 0);
    page->bump_offset = 0;
    page->bump_end = 0;
    seg->pages_used--;
}

/* Get segment from any pointer within it (registry-safe lookup) */
segment_t *ptr_to_segment(const void *ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t base = addr & SEGMENT_MASK;
    if (!segment_is_registered(base)) return NULL;
    segment_t *seg = (segment_t *)base;
    return seg;
}

/* Get page metadata from any pointer within a segment */
page_meta_t *ptr_to_page(const void *ptr) {
    segment_t *seg = ptr_to_segment(ptr);
    if (!seg) return NULL;
    uintptr_t offset = (uintptr_t)ptr - (uintptr_t)seg;
    uint16_t page_idx = (uint16_t)(offset / PAGE_SIZE_ALLOC);
    if (page_idx >= PAGES_PER_SEGMENT) return NULL;
    return &seg->pages[page_idx];
}

/* Get the data start address for a page */
void *page_start(page_meta_t *page) {
    uintptr_t seg_base = (uintptr_t)page->segment;
    return (void *)(seg_base + (size_t)page->page_index * PAGE_SIZE_ALLOC);
}
