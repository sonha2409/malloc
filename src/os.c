#include "malloc_types.h"
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

/*
 * Allocate `size` bytes with the given alignment using mmap.
 * We over-allocate and trim to get the desired alignment.
 */
void *os_mmap_aligned(size_t size, size_t alignment) {
    if (alignment <= (size_t)getpagesize()) {
        void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            atomic_fetch_add_explicit(&g_state.mmap_bytes, size, memory_order_relaxed);
        }
        return (p == MAP_FAILED) ? NULL : p;
    }

    /* Over-allocate to guarantee alignment */
    size_t extra = size + alignment - 1;
    void *base = mmap(NULL, extra, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;

    uintptr_t addr = (uintptr_t)base;
    uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);

    /* Trim leading waste */
    if (aligned > addr) {
        munmap(base, aligned - addr);
    }
    /* Trim trailing waste */
    uintptr_t end = aligned + size;
    uintptr_t base_end = addr + extra;
    if (base_end > end) {
        munmap((void *)end, base_end - end);
    }

    atomic_fetch_add_explicit(&g_state.mmap_bytes, size, memory_order_relaxed);
    return (void *)aligned;
}

void os_munmap(void *addr, size_t size) {
    if (addr && size > 0) {
        munmap(addr, size);
        atomic_fetch_sub_explicit(&g_state.mmap_bytes, size, memory_order_relaxed);
    }
}

void os_madvise_free(void *addr, size_t size) {
    if (addr && size > 0) {
#ifdef MADV_FREE
        madvise(addr, size, MADV_FREE);
#else
        madvise(addr, size, MADV_DONTNEED);
#endif
    }
}

size_t os_page_size(void) {
    return (size_t)getpagesize();
}
