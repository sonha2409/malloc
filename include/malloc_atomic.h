#ifndef MALLOC_ATOMIC_H
#define MALLOC_ATOMIC_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Atomic helpers for the allocator.
 * We use C11 atomics throughout.
 */

/* Tagged pointer for ABA-safe Treiber stack */
typedef struct {
    _Atomic(uint64_t) value;
} tagged_ptr_t;

#define TAG_BITS    16
#define TAG_SHIFT   48
#define PTR_MASK    ((1ULL << TAG_SHIFT) - 1)

static inline void *tagged_ptr_get(tagged_ptr_t *tp) {
    uint64_t v = atomic_load_explicit(&tp->value, memory_order_acquire);
    return (void *)(v & PTR_MASK);
}

static inline uint16_t tagged_ptr_tag(tagged_ptr_t *tp) {
    uint64_t v = atomic_load_explicit(&tp->value, memory_order_acquire);
    return (uint16_t)(v >> TAG_SHIFT);
}

static inline uint64_t tagged_make(void *ptr, uint16_t tag) {
    return ((uint64_t)tag << TAG_SHIFT) | ((uint64_t)ptr & PTR_MASK);
}

static inline bool tagged_cas(tagged_ptr_t *tp, uint64_t expected, uint64_t desired) {
    return atomic_compare_exchange_weak_explicit(
        &tp->value, &expected, desired,
        memory_order_acq_rel, memory_order_acquire
    );
}

/* Treiber stack push: prepend node to lock-free stack.
 * `next_offset` is offset of the "next" pointer within the node struct. */
static inline void treiber_push(tagged_ptr_t *head, void *node, size_t next_offset) {
    void **node_next = (void **)((char *)node + next_offset);
    for (;;) {
        uint64_t old = atomic_load_explicit(&head->value, memory_order_acquire);
        void *old_ptr = (void *)(old & PTR_MASK);
        uint16_t old_tag = (uint16_t)(old >> TAG_SHIFT);
        *node_next = old_ptr;
        uint64_t desired = tagged_make(node, old_tag + 1);
        if (tagged_cas(head, old, desired)) return;
    }
}

/* Treiber stack pop: remove head node. Returns NULL if empty. */
static inline void *treiber_pop(tagged_ptr_t *head, size_t next_offset) {
    for (;;) {
        uint64_t old = atomic_load_explicit(&head->value, memory_order_acquire);
        void *old_ptr = (void *)(old & PTR_MASK);
        if (!old_ptr) return NULL;
        uint16_t old_tag = (uint16_t)(old >> TAG_SHIFT);
        void *next = *(void **)((char *)old_ptr + next_offset);
        uint64_t desired = tagged_make(next, old_tag + 1);
        if (tagged_cas(head, old, desired)) return old_ptr;
    }
}

/* Atomic exchange: swap head with NULL, return old list (for collecting remote frees) */
static inline void *treiber_collect(tagged_ptr_t *head) {
    for (;;) {
        uint64_t old = atomic_load_explicit(&head->value, memory_order_acquire);
        void *old_ptr = (void *)(old & PTR_MASK);
        if (!old_ptr) return NULL;
        uint16_t old_tag = (uint16_t)(old >> TAG_SHIFT);
        uint64_t desired = tagged_make(NULL, old_tag + 1);
        if (tagged_cas(head, old, desired)) return old_ptr;
    }
}

#endif /* MALLOC_ATOMIC_H */
