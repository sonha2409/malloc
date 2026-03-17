#include "malloc_types.h"
#include <string.h>
#include <errno.h>

/*
 * Public malloc API implementation.
 * These are the entry points that either the zone or direct linking calls.
 */

void *my_malloc(size_t size) {
    malloc_ensure_init();

    if (__builtin_expect(size == 0, 0)) size = 1;

    if (size > MEDIUM_MAX) {
        return large_alloc(size);
    }

    return slab_alloc(size);
}

void my_free(void *ptr) {
    if (__builtin_expect(!ptr, 0)) return;

    /* Check if it's from the bootstrap buffer */
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t buf_start = (uintptr_t)g_state.bootstrap_buf;
    if (addr >= buf_start && addr < buf_start + BOOTSTRAP_BUF_SIZE) {
        return; /* bootstrap memory is never freed */
    }

    /* Try slab path first (registry lookup is safe, no deref of unmapped memory) */
    segment_t *seg = ptr_to_segment(ptr);
    if (seg) {
        slab_free(ptr);
        return;
    }

    /* Must be a large allocation */
    large_free(ptr);
}

void *my_calloc(size_t count, size_t size) {
    /* Overflow check */
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) {
        errno = ENOMEM;
        return NULL;
    }

    void *ptr = my_malloc(total);
    if (ptr) {
        /*
         * For bump-allocated slots the memory comes from mmap and is
         * already zeroed. We could skip memset in that case, but for
         * correctness (slots from free list may be dirty) we always zero.
         */
        memset(ptr, 0, total);
    }
    return ptr;
}

void *my_realloc(void *ptr, size_t new_size) {
    if (!ptr) return my_malloc(new_size);
    if (new_size == 0) {
        my_free(ptr);
        return NULL;
    }

    malloc_ensure_init();

    /* Determine current allocation size */
    size_t old_size = 0;

    /* Check bootstrap buffer */
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t buf_start = (uintptr_t)g_state.bootstrap_buf;
    if (addr >= buf_start && addr < buf_start + BOOTSTRAP_BUF_SIZE) {
        /* Can't know exact bootstrap alloc size, just copy conservatively */
        void *new_ptr = my_malloc(new_size);
        if (new_ptr) {
            memcpy(new_ptr, ptr, new_size); /* may over-read, but bootstrap is small */
        }
        return new_ptr;
    }

    /* Check slab allocation */
    segment_t *seg = ptr_to_segment(ptr);
    if (seg) {
        page_meta_t *page = ptr_to_page(ptr);
        if (page) {
            old_size = page->slot_size;

            /* Same size class — no-op realloc */
            if (new_size <= old_size) {
                uint8_t new_bin = size_to_bin(new_size);
                if (new_bin == page->bin_idx) {
                    return ptr; /* fits in same slot */
                }
            }

            /* Different size class: alloc + copy + free */
            void *new_ptr = my_malloc(new_size);
            if (!new_ptr) return NULL;
            size_t copy_size = old_size < new_size ? old_size : new_size;
            memcpy(new_ptr, ptr, copy_size);
            my_free(ptr);
            return new_ptr;
        }
    }

    /* Large allocation */
    large_alloc_t *la = large_find(ptr);
    if (la) {
        old_size = la->user_size;
        if (new_size <= la->size - ((uintptr_t)ptr - (uintptr_t)la->base)) {
            /* Fits in existing mmap — just update user_size */
            la->user_size = new_size;
            return ptr;
        }

        void *new_ptr = my_malloc(new_size);
        if (!new_ptr) return NULL;
        memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
        my_free(ptr);
        return new_ptr;
    }

    return NULL; /* unknown pointer */
}

void *my_memalign(size_t alignment, size_t size) {
    malloc_ensure_init();

    /* Validate alignment is power of 2 */
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        errno = EINVAL;
        return NULL;
    }

    if (alignment <= MIN_ALIGN) {
        return my_malloc(size);
    }

    /* For large alignments, use the large alloc path */
    if (size > MEDIUM_MAX || alignment > PAGE_SIZE_ALLOC) {
        return large_alloc_aligned(size, alignment);
    }

    /*
     * For slab allocations with larger alignment:
     * Find a size class where slot_size is a multiple of alignment.
     * Since slots start at page-aligned addresses, we need slot_size >= alignment.
     */
    size_t alloc_size = size;
    if (alloc_size < alignment) alloc_size = alignment;
    /* Round up to multiple of alignment */
    alloc_size = (alloc_size + alignment - 1) & ~(alignment - 1);

    if (alloc_size > MEDIUM_MAX) {
        return large_alloc_aligned(size, alignment);
    }

    return slab_alloc(alloc_size);
}

int my_posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0) {
        return EINVAL;
    }

    void *ptr = my_memalign(alignment, size);
    if (!ptr) return ENOMEM;
    *memptr = ptr;
    return 0;
}

void *my_valloc(size_t size) {
    return my_memalign(os_page_size(), size);
}

size_t my_malloc_usable_size(const void *ptr) {
    if (!ptr) return 0;

    segment_t *seg = ptr_to_segment(ptr);
    if (seg) {
        page_meta_t *page = ptr_to_page(ptr);
        if (page) return page->slot_size;
    }

    return large_usable_size(ptr);
}
