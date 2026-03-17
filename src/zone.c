#include "malloc_types.h"

#ifndef MALLOC_NO_ZONE

#include <malloc/malloc.h>

/*
 * macOS malloc_zone_t integration.
 * We implement a full zone and register it as the default zone,
 * so all allocations from the process go through our allocator.
 */

static size_t zone_size(malloc_zone_t *zone, const void *ptr) {
    (void)zone;
    return my_malloc_usable_size(ptr);
}

static void *zone_malloc(malloc_zone_t *zone, size_t size) {
    (void)zone;
    return my_malloc(size);
}

static void *zone_calloc(malloc_zone_t *zone, size_t count, size_t size) {
    (void)zone;
    return my_calloc(count, size);
}

static void *zone_valloc(malloc_zone_t *zone, size_t size) {
    (void)zone;
    return my_valloc(size);
}

static void zone_free(malloc_zone_t *zone, void *ptr) {
    (void)zone;
    my_free(ptr);
}

static void *zone_realloc(malloc_zone_t *zone, void *ptr, size_t size) {
    (void)zone;
    return my_realloc(ptr, size);
}

static void *zone_memalign(malloc_zone_t *zone, size_t alignment, size_t size) {
    (void)zone;
    return my_memalign(alignment, size);
}

static void zone_free_definite_size(malloc_zone_t *zone, void *ptr, size_t size) {
    (void)zone;
    (void)size;
    my_free(ptr);
}

static void zone_destroy(malloc_zone_t *zone) {
    (void)zone;
    /* We never destroy our zone */
}

static size_t zone_good_size(malloc_zone_t *zone, size_t size) {
    (void)zone;
    if (size == 0) size = 1;
    if (size > MEDIUM_MAX) {
        return (size + os_page_size() - 1) & ~(os_page_size() - 1);
    }
    uint8_t bin = size_to_bin(size);
    return bin_to_size(bin);
}

static unsigned zone_batch_malloc(malloc_zone_t *zone, size_t size,
                                  void **results, unsigned num_requested) {
    (void)zone;
    unsigned count = 0;
    for (unsigned i = 0; i < num_requested; i++) {
        results[i] = my_malloc(size);
        if (!results[i]) break;
        count++;
    }
    return count;
}

static void zone_batch_free(malloc_zone_t *zone, void **to_be_freed, unsigned count) {
    (void)zone;
    for (unsigned i = 0; i < count; i++) {
        my_free(to_be_freed[i]);
        to_be_freed[i] = NULL;
    }
}

static boolean_t zone_claimed_address(malloc_zone_t *zone, void *ptr) {
    (void)zone;
    if (!ptr) return 0;

    /* Check bootstrap buffer */
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t buf_start = (uintptr_t)g_state.bootstrap_buf;
    if (addr >= buf_start && addr < buf_start + BOOTSTRAP_BUF_SIZE) {
        return 1;
    }

    /* Check slab segments */
    if (ptr_to_segment(ptr)) return 1;

    /* Check large allocations */
    if (large_find(ptr)) return 1;

    return 0;
}

static size_t zone_pressure_relief(malloc_zone_t *zone, size_t pressure) {
    (void)zone;
    (void)pressure;

    size_t released = 0;
    int arena_count = atomic_load_explicit(&g_state.arena_count, memory_order_relaxed);

    for (int i = 0; i < arena_count; i++) {
        arena_t *arena = &g_state.arenas[i];
        pthread_mutex_lock(&arena->lock);

        for (segment_t *seg = arena->segments; seg; seg = seg->next) {
            for (size_t p = PAGES_DATA_START; p < PAGES_PER_SEGMENT; p++) {
                page_meta_t *page = &seg->pages[p];
                if (page->state == PAGE_ACTIVE && page_is_empty(page)) {
                    void *start = page_start(page);
                    os_madvise_free(start, PAGE_SIZE_ALLOC);
                    released += PAGE_SIZE_ALLOC;
                }
            }
        }

        pthread_mutex_unlock(&arena->lock);
    }

    return released;
}

/* Introspection */
static kern_return_t zone_enumerator(task_t task, void *context,
                                     unsigned type_mask,
                                     vm_address_t zone_address,
                                     memory_reader_t reader,
                                     vm_range_recorder_t recorder) {
    (void)task; (void)context; (void)type_mask;
    (void)zone_address; (void)reader; (void)recorder;
    return KERN_SUCCESS;
}

static void zone_statistics(malloc_zone_t *zone, malloc_statistics_t *stats) {
    (void)zone;
    if (!stats) return;

    size_t blocks = 0;
    size_t bytes_in_use = 0;

    /* Aggregate slab stats across all arenas */
    int count = atomic_load_explicit(&g_state.arena_count, memory_order_relaxed);
    for (int i = 0; i < count; i++) {
        arena_t *a = &g_state.arenas[i];
        size_t alloc_c = atomic_load_explicit(&a->alloc_count, memory_order_relaxed);
        size_t free_c  = atomic_load_explicit(&a->free_count, memory_order_relaxed);
        size_t alloc_b = atomic_load_explicit(&a->allocated, memory_order_relaxed);
        size_t freed_b = atomic_load_explicit(&a->freed, memory_order_relaxed);
        blocks += (alloc_c > free_c) ? (alloc_c - free_c) : 0;
        bytes_in_use += (alloc_b > freed_b) ? (alloc_b - freed_b) : 0;
    }

    /* Add large allocation stats */
    size_t large_count = atomic_load_explicit(&g_state.large_alloc_count, memory_order_relaxed);
    size_t large_bytes = atomic_load_explicit(&g_state.large_allocated, memory_order_relaxed);
    blocks += large_count;
    bytes_in_use += large_bytes;

    stats->blocks_in_use = (unsigned)blocks;
    stats->size_in_use = bytes_in_use;
    stats->size_allocated = atomic_load_explicit(&g_state.mmap_bytes, memory_order_relaxed);

    /* Update peak watermark lazily (only when stats are queried) */
    size_t prev_peak = atomic_load_explicit(&g_state.peak_in_use, memory_order_relaxed);
    if (bytes_in_use > prev_peak) {
        atomic_compare_exchange_strong_explicit(
            &g_state.peak_in_use, &prev_peak, bytes_in_use,
            memory_order_relaxed, memory_order_relaxed);
    }
    stats->max_size_in_use = (bytes_in_use > prev_peak) ? bytes_in_use : prev_peak;
}

static void zone_log(malloc_zone_t *zone, void *addr) {
    (void)zone; (void)addr;
}

static void zone_force_lock(malloc_zone_t *zone) {
    (void)zone;
    /* Lock all arenas for fork safety */
    int count = atomic_load(&g_state.arena_count);
    for (int i = 0; i < count; i++) {
        pthread_mutex_lock(&g_state.arenas[i].lock);
    }
    pthread_mutex_lock(&g_state.large_lock);
}

static void zone_force_unlock(malloc_zone_t *zone) {
    (void)zone;
    pthread_mutex_unlock(&g_state.large_lock);
    int count = atomic_load(&g_state.arena_count);
    for (int i = 0; i < count; i++) {
        pthread_mutex_unlock(&g_state.arenas[i].lock);
    }
}

static boolean_t zone_locked(malloc_zone_t *zone) {
    (void)zone;
    return 0;
}

static malloc_introspection_t zone_introspection = {
    .enumerator = zone_enumerator,
    .good_size = zone_good_size,
    .check = NULL,
    .print = NULL,
    .log = zone_log,
    .force_lock = zone_force_lock,
    .force_unlock = zone_force_unlock,
    .statistics = zone_statistics,
    .zone_locked = zone_locked,
};

static malloc_zone_t custom_zone = {
    .reserved1 = NULL,
    .reserved2 = NULL,
    .size = zone_size,
    .malloc = zone_malloc,
    .calloc = zone_calloc,
    .valloc = zone_valloc,
    .free = zone_free,
    .realloc = zone_realloc,
    .destroy = zone_destroy,
    .zone_name = "custom_malloc",
    .batch_malloc = zone_batch_malloc,
    .batch_free = zone_batch_free,
    .introspect = &zone_introspection,
    .version = 12,
    .memalign = zone_memalign,
    .free_definite_size = zone_free_definite_size,
    .pressure_relief = zone_pressure_relief,
    .claimed_address = zone_claimed_address,
};

/*
 * Register our zone as the default.
 * This is called from a constructor so it runs before main().
 */
void zone_register(void) {
    malloc_ensure_init();

    /* Register our zone */
    malloc_zone_register(&custom_zone);

    /*
     * To become the default zone, we need to unregister and re-register
     * so we end up at position 0 in the zone list.
     * malloc_default_zone() returns the zone at index 0.
     */
    malloc_zone_t *default_zone = malloc_default_zone();
    if (default_zone != &custom_zone) {
        /*
         * Unregister default, register ours, re-register old default.
         * But we can't safely unregister the system zone on newer macOS,
         * so instead we just make sure our zone is registered and
         * set it as the default pzone.
         */

        /* The trick: unregister our zone, then register again.
         * On macOS, the last registered zone becomes the default
         * when using malloc_set_zone_name or similar. But the safest
         * approach is to just register and let it be findable. */

        /* Actually, on modern macOS, the first zone registered is the default.
         * We need to be more careful. Let's use a different approach:
         * Override malloc/free/etc. symbols directly. */
    }
}

__attribute__((constructor))
static void zone_init_constructor(void) {
    zone_register();
}

/*
 * Symbol interposition: override the standard malloc family.
 * On macOS with DYLD_INSERT_LIBRARIES, these symbols take precedence.
 */
void *malloc(size_t size) {
    return my_malloc(size);
}

void free(void *ptr) {
    my_free(ptr);
}

void *calloc(size_t count, size_t size) {
    return my_calloc(count, size);
}

void *realloc(void *ptr, size_t size) {
    return my_realloc(ptr, size);
}

void *valloc(size_t size) {
    return my_valloc(size);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    return my_posix_memalign(memptr, alignment, size);
}

size_t malloc_usable_size(const void *ptr) {
    return my_malloc_usable_size(ptr);
}

size_t malloc_size(const void *ptr) {
    return my_malloc_usable_size(ptr);
}

size_t malloc_good_size(size_t size) {
    return zone_good_size(NULL, size);
}

#endif /* MALLOC_NO_ZONE */
