#include "malloc_types.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define ITERATIONS 1000000
#define NUM_THREADS 4

static double time_diff_ms(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 +
           (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

static void bench_small_alloc_free(void) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < ITERATIONS; i++) {
        void *p = my_malloc(64);
        my_free(p);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = time_diff_ms(&start, &end);
    printf("  small alloc+free (%d):    %.2f ms  (%.0f ops/sec)\n",
           ITERATIONS, ms, ITERATIONS / (ms / 1000.0));
}

static void bench_various_sizes(void) {
    struct timespec start, end;
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 4096};
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < ITERATIONS; i++) {
        void *p = my_malloc(sizes[i % nsizes]);
        my_free(p);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = time_diff_ms(&start, &end);
    printf("  varied alloc+free (%d):   %.2f ms  (%.0f ops/sec)\n",
           ITERATIONS, ms, ITERATIONS / (ms / 1000.0));
}

static void bench_batch(void) {
    struct timespec start, end;
    #define BATCH 1000
    void *ptrs[BATCH];

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int r = 0; r < ITERATIONS / BATCH; r++) {
        for (int i = 0; i < BATCH; i++) {
            ptrs[i] = my_malloc(64);
        }
        for (int i = 0; i < BATCH; i++) {
            my_free(ptrs[i]);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = time_diff_ms(&start, &end);
    int total = (ITERATIONS / BATCH) * BATCH;
    printf("  batch alloc+free (%d):    %.2f ms  (%.0f ops/sec)\n",
           total, ms, total / (ms / 1000.0));
    #undef BATCH
}

static void *thread_bench(void *arg) {
    (void)arg;
    int iters = ITERATIONS / NUM_THREADS;
    for (int i = 0; i < iters; i++) {
        void *p = my_malloc(64);
        my_free(p);
    }
    return NULL;
}

static void bench_threaded(void) {
    struct timespec start, end;
    pthread_t threads[NUM_THREADS];

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, thread_bench, NULL);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = time_diff_ms(&start, &end);
    printf("  threaded (%d threads, %d total): %.2f ms  (%.0f ops/sec)\n",
           NUM_THREADS, ITERATIONS, ms, ITERATIONS / (ms / 1000.0));
}

int main(void) {
    malloc_init();

    printf("=== Throughput Benchmark ===\n");
    bench_small_alloc_free();
    bench_various_sizes();
    bench_batch();
    bench_threaded();
    printf("Done.\n");
    return 0;
}
