#include "malloc_types.h"
#include <string.h>

/*
 * Large allocations (> 128KB): direct mmap, tracked in a global doubly-linked list.
 * The large_alloc_t header is placed at the start of the mmap region,
 * and the user pointer follows (aligned).
 */

#define LARGE_HEADER_SIZE  ((sizeof(large_alloc_t) + MIN_ALIGN - 1) & ~(MIN_ALIGN - 1))

void *large_alloc(size_t size) {
    return large_alloc_aligned(size, MIN_ALIGN);
}

void *large_alloc_aligned(size_t size, size_t alignment) {
    if (alignment < MIN_ALIGN) alignment = MIN_ALIGN;

    size_t header_space = (LARGE_HEADER_SIZE + alignment - 1) & ~(alignment - 1);
    size_t total = header_space + size;

    /* Round up to OS page size */
    size_t os_page = os_page_size();
    total = (total + os_page - 1) & ~(os_page - 1);

    void *base = os_mmap_aligned(total, os_page > alignment ? os_page : alignment);
    if (!base) return NULL;

    large_alloc_t *la = (large_alloc_t *)base;
    la->base = base;
    la->size = total;
    la->user_size = size;

    /* Link into global list */
    pthread_mutex_lock(&g_state.large_lock);
    la->next = g_state.large_list;
    la->prev = NULL;
    if (g_state.large_list) {
        g_state.large_list->prev = la;
    }
    g_state.large_list = la;
    pthread_mutex_unlock(&g_state.large_lock);

    return (char *)base + header_space;
}

void large_free(void *ptr) {
    if (!ptr) return;

    pthread_mutex_lock(&g_state.large_lock);

    /* Find the allocation — walk backwards from ptr to find header */
    large_alloc_t *la = g_state.large_list;
    while (la) {
        uintptr_t base = (uintptr_t)la->base;
        uintptr_t end = base + la->size;
        if ((uintptr_t)ptr >= base && (uintptr_t)ptr < end) {
            /* Found it — unlink */
            if (la->prev) la->prev->next = la->next;
            if (la->next) la->next->prev = la->prev;
            if (g_state.large_list == la) g_state.large_list = la->next;

            pthread_mutex_unlock(&g_state.large_lock);
            os_munmap(la->base, la->size);
            return;
        }
        la = la->next;
    }

    pthread_mutex_unlock(&g_state.large_lock);
    /* ptr not found in large list — possible double free or corruption */
}

large_alloc_t *large_find(const void *ptr) {
    pthread_mutex_lock(&g_state.large_lock);
    large_alloc_t *la = g_state.large_list;
    while (la) {
        uintptr_t base = (uintptr_t)la->base;
        uintptr_t end = base + la->size;
        if ((uintptr_t)ptr >= base && (uintptr_t)ptr < end) {
            pthread_mutex_unlock(&g_state.large_lock);
            return la;
        }
        la = la->next;
    }
    pthread_mutex_unlock(&g_state.large_lock);
    return NULL;
}

size_t large_usable_size(const void *ptr) {
    large_alloc_t *la = large_find(ptr);
    if (la) return la->user_size;
    return 0;
}
