#include "malloc_types.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-40s", #name); name(); printf("PASS\n"); } while(0)

TEST(test_default_alignment) {
    for (int i = 0; i < 100; i++) {
        void *p = my_malloc(1 + i);
        assert(p != NULL);
        assert(((uintptr_t)p % MIN_ALIGN) == 0);
        my_free(p);
    }
}

TEST(test_memalign_16) {
    void *p = my_memalign(16, 100);
    assert(p != NULL);
    assert(((uintptr_t)p % 16) == 0);
    my_free(p);
}

TEST(test_memalign_32) {
    void *p = my_memalign(32, 100);
    assert(p != NULL);
    assert(((uintptr_t)p % 32) == 0);
    my_free(p);
}

TEST(test_memalign_64) {
    void *p = my_memalign(64, 100);
    assert(p != NULL);
    assert(((uintptr_t)p % 64) == 0);
    my_free(p);
}

TEST(test_memalign_4096) {
    void *p = my_memalign(4096, 100);
    assert(p != NULL);
    assert(((uintptr_t)p % 4096) == 0);
    my_free(p);
}

TEST(test_posix_memalign) {
    void *p = NULL;
    int ret = my_posix_memalign(&p, 256, 1024);
    assert(ret == 0);
    assert(p != NULL);
    assert(((uintptr_t)p % 256) == 0);
    my_free(p);
}

TEST(test_posix_memalign_invalid) {
    void *p = NULL;
    /* alignment not power of 2 */
    int ret = my_posix_memalign(&p, 3, 100);
    assert(ret != 0);

    /* alignment < sizeof(void*) */
    ret = my_posix_memalign(&p, 2, 100);
    assert(ret != 0);
}

TEST(test_valloc) {
    void *p = my_valloc(100);
    assert(p != NULL);
    assert(((uintptr_t)p % os_page_size()) == 0);
    my_free(p);
}

TEST(test_memalign_large) {
    void *p = my_memalign(65536, 256 * 1024);
    assert(p != NULL);
    assert(((uintptr_t)p % 65536) == 0);
    memset(p, 0, 256 * 1024);
    my_free(p);
}

int main(void) {
    malloc_init();

    printf("=== Alignment Tests ===\n");
    RUN(test_default_alignment);
    RUN(test_memalign_16);
    RUN(test_memalign_32);
    RUN(test_memalign_64);
    RUN(test_memalign_4096);
    RUN(test_posix_memalign);
    RUN(test_posix_memalign_invalid);
    RUN(test_valloc);
    RUN(test_memalign_large);
    printf("All alignment tests passed!\n");
    return 0;
}
