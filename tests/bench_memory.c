/*
 * F6.5: Memory efficiency benchmark
 * Measures RSS vs requested bytes under various workloads.
 * Uses mach_task_info for accurate RSS on macOS.
 */
#include "malloc_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/task_info.h>

static size_t get_rss_bytes(void) {
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) {
        return 0;
    }
    return info.resident_size;
}

static void print_efficiency(const char *label, size_t requested, size_t rss_before, size_t rss_after) {
    size_t rss_delta = (rss_after > rss_before) ? (rss_after - rss_before) : 0;
    double overhead = (rss_delta > 0 && requested > 0)
        ? ((double)rss_delta / (double)requested - 1.0) * 100.0
        : 0.0;
    printf("  %-35s requested: %8zu KB  RSS delta: %8zu KB  overhead: %+.1f%%\n",
           label, requested / 1024, rss_delta / 1024, overhead);
}

/*
 * Workload 1: Many small allocations (64 bytes)
 */
static void bench_many_small(void) {
    const int N = 100000;
    const size_t SIZE = 64;
    size_t requested = N * SIZE;

    size_t rss_before = get_rss_bytes();

    void **ptrs = (void **)my_malloc(N * sizeof(void *));
    for (int i = 0; i < N; i++) {
        ptrs[i] = my_malloc(SIZE);
        memset(ptrs[i], 0xAB, SIZE); /* touch pages */
    }

    size_t rss_after = get_rss_bytes();
    print_efficiency("many small (100K x 64B)", requested, rss_before, rss_after);

    for (int i = 0; i < N; i++) my_free(ptrs[i]);
    my_free(ptrs);
}

/*
 * Workload 2: Mixed sizes (16B to 16KB)
 */
static void bench_mixed_sizes(void) {
    const int N = 50000;
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);
    size_t requested = 0;

    size_t rss_before = get_rss_bytes();

    void **ptrs = (void **)my_malloc(N * sizeof(void *));
    for (int i = 0; i < N; i++) {
        size_t sz = sizes[i % nsizes];
        ptrs[i] = my_malloc(sz);
        memset(ptrs[i], 0xCD, sz);
        requested += sz;
    }

    size_t rss_after = get_rss_bytes();
    print_efficiency("mixed sizes (50K allocs)", requested, rss_before, rss_after);

    for (int i = 0; i < N; i++) my_free(ptrs[i]);
    my_free(ptrs);
}

/*
 * Workload 3: Large allocations (64KB to 1MB)
 */
static void bench_large(void) {
    const int N = 100;
    size_t sizes[] = {65536, 131072, 262144, 524288, 1048576};
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);
    size_t requested = 0;

    size_t rss_before = get_rss_bytes();

    void **ptrs = (void **)my_malloc(N * sizeof(void *));
    for (int i = 0; i < N; i++) {
        size_t sz = sizes[i % nsizes];
        ptrs[i] = my_malloc(sz);
        memset(ptrs[i], 0xEF, sz);
        requested += sz;
    }

    size_t rss_after = get_rss_bytes();
    print_efficiency("large (100 x 64KB-1MB)", requested, rss_before, rss_after);

    for (int i = 0; i < N; i++) my_free(ptrs[i]);
    my_free(ptrs);
}

/*
 * Workload 4: Alloc-heavy then free-heavy (sawtooth)
 * Allocate N, free half, allocate N more, measure.
 */
static void bench_sawtooth(void) {
    const int N = 50000;
    const size_t SIZE = 128;

    size_t rss_before = get_rss_bytes();

    /* Phase 1: allocate N */
    void **ptrs1 = (void **)my_malloc(N * sizeof(void *));
    for (int i = 0; i < N; i++) {
        ptrs1[i] = my_malloc(SIZE);
        memset(ptrs1[i], 0x11, SIZE);
    }

    /* Phase 2: free first half */
    for (int i = 0; i < N / 2; i++) {
        my_free(ptrs1[i]);
        ptrs1[i] = NULL;
    }

    /* Phase 3: allocate N more (should reuse freed slots) */
    void **ptrs2 = (void **)my_malloc(N * sizeof(void *));
    for (int i = 0; i < N; i++) {
        ptrs2[i] = my_malloc(SIZE);
        memset(ptrs2[i], 0x22, SIZE);
    }

    size_t rss_after = get_rss_bytes();
    /* Active: N/2 from ptrs1 + N from ptrs2 = 1.5N */
    size_t requested = (size_t)(N + N / 2) * SIZE;
    print_efficiency("sawtooth (reuse test, 75K live)", requested, rss_before, rss_after);

    for (int i = 0; i < N; i++) {
        if (ptrs1[i]) my_free(ptrs1[i]);
        my_free(ptrs2[i]);
    }
    my_free(ptrs1);
    my_free(ptrs2);
}

/*
 * Workload 5: Fragmentation stress — alternating alloc/free of different sizes
 */
static void bench_fragmentation(void) {
    const int N = 100000;
    size_t requested = 0;

    size_t rss_before = get_rss_bytes();

    void **ptrs = (void **)my_malloc(N * sizeof(void *));
    /* Allocate with varying sizes */
    for (int i = 0; i < N; i++) {
        size_t sz = 16 + (i % 64) * 16; /* 16 to 1024 in steps of 16 */
        ptrs[i] = my_malloc(sz);
        memset(ptrs[i], 0x33, sz);
        requested += sz;
    }

    /* Free every other one */
    for (int i = 0; i < N; i += 2) {
        my_free(ptrs[i]);
        ptrs[i] = NULL;
        /* requested stays: we report peak */
    }

    /* Reallocate into the holes with different sizes */
    size_t requested_live = 0;
    for (int i = 0; i < N; i++) {
        if (ptrs[i] == NULL) {
            size_t sz = 32 + (i % 32) * 32; /* different pattern */
            ptrs[i] = my_malloc(sz);
            memset(ptrs[i], 0x44, sz);
            requested_live += sz;
        } else {
            size_t sz = 16 + (i % 64) * 16;
            requested_live += sz;
        }
    }

    size_t rss_after = get_rss_bytes();
    print_efficiency("fragmentation stress (100K)", requested_live, rss_before, rss_after);

    for (int i = 0; i < N; i++) my_free(ptrs[i]);
    my_free(ptrs);
}

int main(void) {
    printf("=== Memory Efficiency Benchmark (F6.5) ===\n");
    printf("  (RSS measured via mach_task_info)\n\n");

    bench_many_small();
    bench_mixed_sizes();
    bench_large();
    bench_sawtooth();
    bench_fragmentation();

    printf("\nDone.\n");
    return 0;
}
