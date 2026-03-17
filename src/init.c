#include "malloc_types.h"
#include <string.h>

/* Global state — single instance */
global_state_t g_state;

void *bootstrap_alloc(size_t size) {
    size = (size + MIN_ALIGN - 1) & ~(MIN_ALIGN - 1);
    size_t offset = atomic_fetch_add(&g_state.bootstrap_used, size);
    if (offset + size > BOOTSTRAP_BUF_SIZE) return NULL;
    return &g_state.bootstrap_buf[offset];
}

bool malloc_is_initialized(void) {
    return atomic_load(&g_state.initialized) == 2;
}

void malloc_init(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_state.initialized, &expected, 1)) {
        /* Another thread is initializing or already done */
        while (atomic_load(&g_state.initialized) != 2) {
            /* Spin wait */
        }
        return;
    }

    /* Initialize locks */
    pthread_mutex_init(&g_state.init_lock, NULL);
    pthread_mutex_init(&g_state.large_lock, NULL);
    pthread_mutex_init(&g_state.segment_registry_lock, NULL);

    /* Zero out segment registry */
    for (int i = 0; i < SEGMENT_REGISTRY_SIZE; i++) {
        atomic_store(&g_state.segment_registry[i], (uintptr_t)0);
    }

    /* Initialize size class tables */
    size_class_init();

    /* Initialize TLD system */
    tld_init();

    /* Create initial arenas */
    int num_arenas = 4; /* Start with 4, expand as needed */
    if (num_arenas > MAX_ARENAS) num_arenas = MAX_ARENAS;
    for (int i = 0; i < num_arenas; i++) {
        arena_init(&g_state.arenas[i], (uint32_t)i);
    }
    atomic_store(&g_state.arena_count, num_arenas);
    atomic_store(&g_state.arena_next, 0);

    g_state.large_list = NULL;
    atomic_store(&g_state.bootstrap_used, 0);

    /* Mark as fully initialized */
    atomic_store(&g_state.initialized, 2);
}

void malloc_ensure_init(void) {
    if (__builtin_expect(atomic_load(&g_state.initialized) != 2, 0)) {
        malloc_init();
    }
}
