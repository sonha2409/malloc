#include "malloc_types.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-40s", #name); name(); printf("PASS\n"); } while(0)

TEST(test_realloc_null) {
    /* realloc(NULL, size) == malloc(size) */
    void *p = my_realloc(NULL, 64);
    assert(p != NULL);
    my_free(p);
}

TEST(test_realloc_zero) {
    /* realloc(ptr, 0) == free(ptr) */
    void *p = my_malloc(64);
    assert(p != NULL);
    void *q = my_realloc(p, 0);
    assert(q == NULL);
}

TEST(test_realloc_grow) {
    char *p = (char *)my_malloc(32);
    assert(p != NULL);
    memset(p, 'A', 32);

    char *q = (char *)my_realloc(p, 256);
    assert(q != NULL);
    /* First 32 bytes should be preserved */
    for (int i = 0; i < 32; i++) {
        assert(q[i] == 'A');
    }
    my_free(q);
}

TEST(test_realloc_shrink) {
    char *p = (char *)my_malloc(256);
    assert(p != NULL);
    memset(p, 'B', 256);

    char *q = (char *)my_realloc(p, 32);
    assert(q != NULL);
    for (int i = 0; i < 32; i++) {
        assert(q[i] == 'B');
    }
    my_free(q);
}

TEST(test_realloc_same_class) {
    /* Reallocing within the same size class should return same pointer */
    void *p = my_malloc(24);
    void *q = my_realloc(p, 28);
    /* Same bin → likely same pointer (slot size covers both) */
    /* Just verify it works */
    assert(q != NULL);
    my_free(q);
}

TEST(test_realloc_chain) {
    char *p = (char *)my_malloc(8);
    memset(p, 'X', 8);

    for (size_t sz = 16; sz <= 4096; sz *= 2) {
        p = (char *)my_realloc(p, sz);
        assert(p != NULL);
        /* First 8 bytes should still be 'X' */
        for (int i = 0; i < 8; i++) {
            assert(p[i] == 'X');
        }
        memset(p + sz / 2, 'Y', sz / 2);
    }
    my_free(p);
}

int main(void) {
    malloc_init();

    printf("=== Realloc Tests ===\n");
    RUN(test_realloc_null);
    RUN(test_realloc_zero);
    RUN(test_realloc_grow);
    RUN(test_realloc_shrink);
    RUN(test_realloc_same_class);
    RUN(test_realloc_chain);
    printf("All realloc tests passed!\n");
    return 0;
}
