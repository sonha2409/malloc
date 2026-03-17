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
    RUN(test_size_classes);
    printf("All basic tests passed!\n");
    return 0;
}
