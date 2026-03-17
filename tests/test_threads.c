#include "malloc_types.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>

#define NUM_THREADS   8
#define ALLOCS_PER    10000
#define MAX_SIZE      2048

static void *thread_alloc_free(void *arg) {
    int tid = *(int *)arg;
    unsigned seed = (unsigned)tid * 12345 + 67890;

    void *ptrs[256];
    int count = 0;

    for (int i = 0; i < ALLOCS_PER; i++) {
        seed = seed * 1103515245 + 12345; /* simple LCG */
        int action = seed % 3;

        if (action != 2 || count == 0) {
            /* Allocate */
            size_t sz = (seed >> 4) % MAX_SIZE + 1;
            void *p = my_malloc(sz);
            assert(p != NULL);
            memset(p, tid & 0xFF, sz < 64 ? sz : 64);
            if (count < 256) {
                ptrs[count++] = p;
            } else {
                my_free(p);
            }
        } else {
            /* Free random */
            int idx = (seed >> 8) % count;
            my_free(ptrs[idx]);
            ptrs[idx] = ptrs[--count];
        }
    }

    /* Free remaining */
    for (int i = 0; i < count; i++) {
        my_free(ptrs[i]);
    }

    return NULL;
}

/* Test cross-thread frees: alloc in one thread, free in another */
static void *ptrs_shared[NUM_THREADS][1024];
static int shared_counts[NUM_THREADS];

static void *thread_alloc_only(void *arg) {
    int tid = *(int *)arg;
    for (int i = 0; i < 1024; i++) {
        ptrs_shared[tid][i] = my_malloc(64);
        assert(ptrs_shared[tid][i] != NULL);
        memset(ptrs_shared[tid][i], tid, 64);
    }
    shared_counts[tid] = 1024;
    return NULL;
}

static void *thread_free_others(void *arg) {
    int tid = *(int *)arg;
    /* Free allocations made by the previous thread */
    int other = (tid + 1) % NUM_THREADS;
    /* Wait for other thread to finish allocating */
    while (shared_counts[other] < 1024) {
        /* spin */
    }
    for (int i = 0; i < 1024; i++) {
        my_free(ptrs_shared[other][i]);
    }
    return NULL;
}

static void test_concurrent_alloc_free(void) {
    printf("  %-40s", "test_concurrent_alloc_free");

    pthread_t threads[NUM_THREADS];
    int ids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, thread_alloc_free, &ids[i]);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    printf("PASS\n");
}

static void test_cross_thread_free(void) {
    printf("  %-40s", "test_cross_thread_free");

    memset(shared_counts, 0, sizeof(shared_counts));

    pthread_t alloc_threads[NUM_THREADS];
    pthread_t free_threads[NUM_THREADS];
    int ids[NUM_THREADS];

    /* Start allocating threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i;
        pthread_create(&alloc_threads[i], NULL, thread_alloc_only, &ids[i]);
    }
    /* Wait for all allocations to complete */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(alloc_threads[i], NULL);
    }

    /* Start freeing threads (each frees another thread's allocations) */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&free_threads[i], NULL, thread_free_others, &ids[i]);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(free_threads[i], NULL);
    }

    printf("PASS\n");
}

static void *realloc_worker(void *arg) {
    (void)arg;
    char *p = (char *)my_malloc(8);
    memset(p, 'A', 8);
    for (size_t sz = 16; sz <= 8192; sz *= 2) {
        p = (char *)my_realloc(p, sz);
        assert(p != NULL);
    }
    my_free(p);
    return NULL;
}

static void test_realloc_threaded(void) {
    printf("  %-40s", "test_realloc_threaded");

    pthread_t threads[4];
    int ids[4];

    for (int i = 0; i < 4; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, realloc_worker, &ids[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("PASS\n");
}

int main(void) {
    malloc_init();

    printf("=== Threading Tests ===\n");
    test_concurrent_alloc_free();
    test_cross_thread_free();
    test_realloc_threaded();
    printf("All threading tests passed!\n");
    return 0;
}
