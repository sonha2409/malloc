#include "malloc_types.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-40s", #name); name(); printf("PASS\n"); } while(0)

TEST(test_small_alloc) {
    void *p = my_malloc(16);
    assert(p != NULL);
    assert(((uintptr_t)p & (MIN_ALIGN - 1)) == 0);
    memset(p, 0xAA, 16);
    my_free(p);
}

TEST(test_various_sizes) {
    size_t sizes[] = {1, 7, 8, 15, 16, 31, 32, 63, 64, 100, 128,
                      256, 512, 1024, 4096, 8192, 32768, 65536, 100000};
    int n = sizeof(sizes) / sizeof(sizes[0]);

    void *ptrs[19];
    for (int i = 0; i < n; i++) {
        ptrs[i] = my_malloc(sizes[i]);
        assert(ptrs[i] != NULL);
        assert(((uintptr_t)ptrs[i] & (MIN_ALIGN - 1)) == 0);
        memset(ptrs[i], (char)i, sizes[i]);
    }
    for (int i = 0; i < n; i++) {
        my_free(ptrs[i]);
    }
}

TEST(test_no_overlap) {
    #define N 1000
    void *ptrs[N];
    for (int i = 0; i < N; i++) {
        ptrs[i] = my_malloc(64);
        assert(ptrs[i] != NULL);
        memset(ptrs[i], i & 0xFF, 64);
    }
    /* Verify no overlap */
    for (int i = 0; i < N; i++) {
        unsigned char *p = (unsigned char *)ptrs[i];
        for (int j = 0; j < 64; j++) {
            assert(p[j] == (unsigned char)(i & 0xFF));
        }
    }
    for (int i = 0; i < N; i++) {
        my_free(ptrs[i]);
    }
    #undef N
}

TEST(test_free_and_reuse) {
    void *p1 = my_malloc(32);
    my_free(p1);
    void *p2 = my_malloc(32);
    assert(p2 != NULL);
    /* p2 might or might not be the same as p1, both are valid */
    my_free(p2);
}

TEST(test_calloc_zeroed) {
    int *p = (int *)my_calloc(100, sizeof(int));
    assert(p != NULL);
    for (int i = 0; i < 100; i++) {
        assert(p[i] == 0);
    }
    my_free(p);
}

TEST(test_calloc_overflow) {
    void *p = my_calloc((size_t)-1, (size_t)-1);
    assert(p == NULL);
}

TEST(test_free_null) {
    my_free(NULL); /* should not crash */
}

TEST(test_malloc_zero) {
    void *p = my_malloc(0);
    assert(p != NULL);
    my_free(p);
}

TEST(test_many_alloc_free_cycles) {
    for (int cycle = 0; cycle < 100; cycle++) {
        void *ptrs[100];
        for (int i = 0; i < 100; i++) {
            ptrs[i] = my_malloc(32 + (i % 64));
            assert(ptrs[i] != NULL);
        }
        for (int i = 0; i < 100; i++) {
            my_free(ptrs[i]);
        }
    }
}

TEST(test_statistics) {
    /* Get baseline stats */
    int arena_count = atomic_load(&g_state.arena_count);
    size_t base_alloc = 0, base_freed = 0;
    size_t base_alloc_count = 0, base_free_count = 0;
    for (int i = 0; i < arena_count; i++) {
        base_alloc += atomic_load_explicit(&g_state.arenas[i].allocated, memory_order_relaxed);
        base_freed += atomic_load_explicit(&g_state.arenas[i].freed, memory_order_relaxed);
        base_alloc_count += atomic_load_explicit(&g_state.arenas[i].alloc_count, memory_order_relaxed);
        base_free_count += atomic_load_explicit(&g_state.arenas[i].free_count, memory_order_relaxed);
    }

    /* Force a TLD stat flush by doing enough ops */
    #define STAT_N 200
    void *ptrs[STAT_N];
    for (int i = 0; i < STAT_N; i++) {
        ptrs[i] = my_malloc(64);
        assert(ptrs[i] != NULL);
    }
    for (int i = 0; i < STAT_N; i++) {
        my_free(ptrs[i]);
    }

    /* Read stats again — should reflect the allocations */
    size_t after_alloc = 0, after_freed = 0;
    size_t after_alloc_count = 0, after_free_count = 0;
    for (int i = 0; i < arena_count; i++) {
        after_alloc += atomic_load_explicit(&g_state.arenas[i].allocated, memory_order_relaxed);
        after_freed += atomic_load_explicit(&g_state.arenas[i].freed, memory_order_relaxed);
        after_alloc_count += atomic_load_explicit(&g_state.arenas[i].alloc_count, memory_order_relaxed);
        after_free_count += atomic_load_explicit(&g_state.arenas[i].free_count, memory_order_relaxed);
    }

    size_t delta_alloc = after_alloc - base_alloc;
    size_t delta_freed = after_freed - base_freed;
    size_t delta_alloc_count = after_alloc_count - base_alloc_count;
    size_t delta_free_count = after_free_count - base_free_count;

    /* We allocated 200 blocks of 64 bytes (slot_size = bin_to_size(size_to_bin(64))).
     * Stats may be slightly behind (up to 64 ops buffered in TLD),
     * but 200 ops forces at least 2 flushes, so most should be visible. */
    uint8_t bin = size_to_bin(64);
    size_t slot = bin_to_size(bin);

    /* At least 128 of 200 allocs should have flushed (200 - 64 buffer = 136 minimum) */
    assert(delta_alloc_count >= STAT_N / 2);
    assert(delta_free_count >= STAT_N / 2);
    assert(delta_alloc >= (STAT_N / 2) * slot);
    assert(delta_freed >= (STAT_N / 2) * slot);

    /* Test large alloc stats */
    size_t large_before = atomic_load_explicit(&g_state.large_alloc_count, memory_order_relaxed);
    void *big = my_malloc(256 * 1024); /* 256KB = large */
    assert(big != NULL);
    size_t large_after = atomic_load_explicit(&g_state.large_alloc_count, memory_order_relaxed);
    assert(large_after == large_before + 1);

    size_t large_bytes = atomic_load_explicit(&g_state.large_allocated, memory_order_relaxed);
    assert(large_bytes > 0);

    my_free(big);
    size_t large_final = atomic_load_explicit(&g_state.large_alloc_count, memory_order_relaxed);
    assert(large_final == large_before);

    /* Test mmap_bytes is positive (we've definitely mmap'd something) */
    size_t mmap = atomic_load_explicit(&g_state.mmap_bytes, memory_order_relaxed);
    assert(mmap > 0);
    #undef STAT_N
}

TEST(test_size_classes) {
    /* Verify size_to_bin and bin_to_size are consistent */
    for (size_t s = 1; s <= MEDIUM_MAX; s += (s < 256 ? 1 : s / 8)) {
        uint8_t bin = size_to_bin(s);
        size_t slot = bin_to_size(bin);
        assert(slot >= s);
        assert(bin < BIN_COUNT);
    }
}

int main(void) {
    malloc_init();

    printf("=== Basic Allocation Tests ===\n");
    RUN(test_small_alloc);
    RUN(test_various_sizes);
    RUN(test_no_overlap);
    RUN(test_free_and_reuse);
    RUN(test_calloc_zeroed);
    RUN(test_calloc_overflow);
    RUN(test_free_null);
    RUN(test_malloc_zero);
    RUN(test_many_alloc_free_cycles);
    RUN(test_statistics);
    RUN(test_size_classes);
    printf("All basic tests passed!\n");
    return 0;
}
