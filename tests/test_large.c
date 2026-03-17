#include "malloc_types.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-40s", #name); name(); printf("PASS\n"); } while(0)

TEST(test_large_alloc) {
    size_t size = 256 * 1024; /* 256KB > MEDIUM_MAX */
    void *p = my_malloc(size);
    assert(p != NULL);
    memset(p, 0xCC, size);
    my_free(p);
}

TEST(test_large_various) {
    size_t sizes[] = {
        128 * 1024 + 1, /* just over medium max */
        256 * 1024,
        1 * 1024 * 1024,
        4 * 1024 * 1024,
        16 * 1024 * 1024,
    };
    int n = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < n; i++) {
        void *p = my_malloc(sizes[i]);
        assert(p != NULL);
        memset(p, i, 1024); /* write first 1KB */
        my_free(p);
    }
}

TEST(test_large_realloc) {
    size_t sz1 = 256 * 1024;
    char *p = (char *)my_malloc(sz1);
    assert(p != NULL);
    memset(p, 'Z', sz1);

    size_t sz2 = 512 * 1024;
    char *q = (char *)my_realloc(p, sz2);
    assert(q != NULL);
    /* First sz1 bytes preserved */
    for (int i = 0; i < 1024; i++) {
        assert(q[i] == 'Z');
    }
    my_free(q);
}

TEST(test_large_multiple) {
    #define N 20
    void *ptrs[N];
    for (int i = 0; i < N; i++) {
        ptrs[i] = my_malloc(256 * 1024 + i * 4096);
        assert(ptrs[i] != NULL);
    }
    for (int i = N - 1; i >= 0; i--) {
        my_free(ptrs[i]);
    }
    #undef N
}

TEST(test_large_usable_size) {
    size_t sz = 300 * 1024;
    void *p = my_malloc(sz);
    assert(p != NULL);
    size_t usable = my_malloc_usable_size(p);
    assert(usable >= sz);
    my_free(p);
}

int main(void) {
    malloc_init();

    printf("=== Large Allocation Tests ===\n");
    RUN(test_large_alloc);
    RUN(test_large_various);
    RUN(test_large_realloc);
    RUN(test_large_multiple);
    RUN(test_large_usable_size);
    printf("All large allocation tests passed!\n");
    return 0;
}
