#include "malloc_types.h"
#include <stdio.h>
#include <string.h>
#undef NDEBUG  /* ensure assert() is active even in Release builds */
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

#if !MALLOC_DEBUG
#error "test_debug.c must be compiled with MALLOC_DEBUG=1"
#endif

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-44s", #name); name(); printf("PASS\n"); } while(0)

/*
 * Helper: run a function in a child process and verify it aborts (SIGABRT).
 * Returns true if the child was killed by SIGABRT.
 */
static bool expect_abort(void (*fn)(void)) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: run the function (should abort) */
        fn();
        _exit(0); /* if we get here, the function didn't abort */
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
        return true;
    }
    return false;
}

/* ── Basic bitmap tracking ── */

TEST(test_bitmap_basic) {
    /* Allocate and free various sizes, then verify heap integrity */
    void *ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = my_malloc(32 + (i % 64));
        assert(ptrs[i] != NULL);
        memset(ptrs[i], 0xBB, 32);
    }
    assert(debug_heap_check());

    for (int i = 0; i < 100; i++) {
        my_free(ptrs[i]);
    }
    assert(debug_heap_check());
}

TEST(test_bitmap_varied_sizes) {
    /* Test across multiple size classes */
    size_t sizes[] = {8, 16, 24, 32, 64, 128, 256, 512, 1024, 4096, 16384, 32768};
    int n = sizeof(sizes) / sizeof(sizes[0]);
    void *ptrs[12];

    for (int i = 0; i < n; i++) {
        ptrs[i] = my_malloc(sizes[i]);
        assert(ptrs[i] != NULL);
    }
    assert(debug_heap_check());

    for (int i = 0; i < n; i++) {
        my_free(ptrs[i]);
    }
    assert(debug_heap_check());
}

TEST(test_bitmap_alloc_free_cycles) {
    /* Multiple cycles of alloc/free with integrity checks */
    for (int cycle = 0; cycle < 50; cycle++) {
        void *ptrs[50];
        for (int i = 0; i < 50; i++) {
            ptrs[i] = my_malloc(64);
            assert(ptrs[i] != NULL);
        }
        assert(debug_heap_check());

        /* Free half */
        for (int i = 0; i < 25; i++) {
            my_free(ptrs[i]);
        }
        assert(debug_heap_check());

        /* Reallocate */
        for (int i = 0; i < 25; i++) {
            ptrs[i] = my_malloc(64);
            assert(ptrs[i] != NULL);
        }
        assert(debug_heap_check());

        /* Free all */
        for (int i = 0; i < 50; i++) {
            my_free(ptrs[i]);
        }
        assert(debug_heap_check());
    }
}

/* ── Double-free detection ── */

static void do_double_free_local(void) {
    malloc_init();
    void *p = my_malloc(64);
    my_free(p);
    my_free(p); /* should abort */
}

TEST(test_double_free_local) {
    assert(expect_abort(do_double_free_local));
}

static void do_double_free_different_sizes(void) {
    malloc_init();
    void *p = my_malloc(128);
    my_free(p);
    my_free(p); /* should abort */
}

TEST(test_double_free_different_size) {
    assert(expect_abort(do_double_free_different_sizes));
}

/* ── Cross-thread double-free detection ── */

static void *thread_free(void *arg) {
    my_free(arg);
    return NULL;
}

static void do_double_free_cross_thread(void) {
    malloc_init();
    void *p = my_malloc(64);

    /* Free from another thread (remote free) */
    pthread_t t;
    pthread_create(&t, NULL, thread_free, p);
    pthread_join(t, NULL);

    /* Free again from main thread — should detect double-free */
    my_free(p);
}

TEST(test_double_free_cross_thread) {
    assert(expect_abort(do_double_free_cross_thread));
}

/* ── Heap integrity after threaded work ── */

struct thread_work {
    int count;
    size_t alloc_size;
};

static void *thread_alloc_free(void *arg) {
    struct thread_work *w = (struct thread_work *)arg;
    void *ptrs[200];
    for (int i = 0; i < w->count; i++) {
        ptrs[i] = my_malloc(w->alloc_size);
        assert(ptrs[i] != NULL);
        memset(ptrs[i], 0xCC, w->alloc_size < 64 ? w->alloc_size : 64);
    }
    for (int i = 0; i < w->count; i++) {
        my_free(ptrs[i]);
    }
    return NULL;
}

TEST(test_heap_check_after_threads) {
    #define NTHREADS 4
    pthread_t threads[NTHREADS];
    struct thread_work work[NTHREADS];

    for (int i = 0; i < NTHREADS; i++) {
        work[i].count = 200;
        work[i].alloc_size = 32 + i * 32;
        pthread_create(&threads[i], NULL, thread_alloc_free, &work[i]);
    }
    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* All threads done — heap should be consistent */
    assert(debug_heap_check());
    #undef NTHREADS
}

/* ── Calloc + realloc with bitmap ── */

TEST(test_calloc_bitmap) {
    int *p = (int *)my_calloc(50, sizeof(int));
    assert(p != NULL);
    for (int i = 0; i < 50; i++) {
        assert(p[i] == 0);
    }
    assert(debug_heap_check());
    my_free(p);
    assert(debug_heap_check());
}

TEST(test_realloc_bitmap) {
    void *p = my_malloc(32);
    assert(p != NULL);
    memset(p, 0xAA, 32);
    assert(debug_heap_check());

    /* Realloc to larger — may allocate new slot */
    void *p2 = my_realloc(p, 256);
    assert(p2 != NULL);
    /* Old slot should be freed, new slot allocated */
    assert(debug_heap_check());

    my_free(p2);
    assert(debug_heap_check());
}

int main(void) {
    malloc_init();

    printf("=== Debug Mode Tests (MALLOC_DEBUG=1) ===\n");
    RUN(test_bitmap_basic);
    RUN(test_bitmap_varied_sizes);
    RUN(test_bitmap_alloc_free_cycles);
    RUN(test_double_free_local);
    RUN(test_double_free_different_size);
    RUN(test_double_free_cross_thread);
    RUN(test_heap_check_after_threads);
    RUN(test_calloc_bitmap);
    RUN(test_realloc_bitmap);
    printf("All debug tests passed!\n");
    return 0;
}
