#include "malloc_types.h"
#include <string.h>

static pthread_key_t tld_key;
static pthread_once_t tld_once = PTHREAD_ONCE_INIT;
static _Atomic(uint32_t) thread_id_counter = 0;

void tld_cleanup(void *arg) {
    tld_t *tld = (tld_t *)arg;
    if (!tld) return;

    if (tld->arena) {
        atomic_fetch_sub(&tld->arena->thread_count, 1);
    }

    /* Don't free if it's from the bootstrap buffer */
    uintptr_t tld_addr = (uintptr_t)tld;
    uintptr_t buf_start = (uintptr_t)g_state.bootstrap_buf;
    uintptr_t buf_end = buf_start + BOOTSTRAP_BUF_SIZE;
    if (tld_addr >= buf_start && tld_addr < buf_end) {
        return; /* bootstrap-allocated, don't free */
    }

    /* Free via OS (we can't call our own free during cleanup easily) */
    os_munmap(tld, os_page_size());
}

static void tld_key_create(void) {
    pthread_key_create(&tld_key, tld_cleanup);
}

void tld_init(void) {
    pthread_once(&tld_once, tld_key_create);
}

tld_t *tld_get(void) {
    tld_init();

    tld_t *tld = (tld_t *)pthread_getspecific(tld_key);
    if (tld) return tld;

    /* Allocate new TLD */
    if (!malloc_is_initialized()) {
        /* Use bootstrap buffer */
        tld = (tld_t *)bootstrap_alloc(sizeof(tld_t));
    } else {
        /* Use OS mmap directly to avoid recursion */
        tld = (tld_t *)os_mmap_aligned(os_page_size(), os_page_size());
    }
    if (!tld) return NULL;

    memset(tld, 0, sizeof(tld_t));
    tld->thread_id = atomic_fetch_add(&thread_id_counter, 1);
    tld->arena = NULL;

    pthread_setspecific(tld_key, tld);
    return tld;
}
