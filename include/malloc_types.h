#ifndef MALLOC_TYPES_H
#define MALLOC_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include "malloc_config.h"
#include "malloc_atomic.h"

/* Forward declarations */
typedef struct segment_s    segment_t;
typedef struct page_meta_s  page_meta_t;
typedef struct arena_s      arena_t;
typedef struct tld_s        tld_t;
typedef struct large_alloc_s large_alloc_t;

/* ──────────────────────────────────────────────
 * Page metadata (out-of-band, stored in segment header)
 * ────────────────────────────────────────────── */
typedef enum {
    PAGE_UNUSED = 0,
    PAGE_ACTIVE,        /* has free slots */
    PAGE_FULL,          /* all slots used */
    PAGE_RETIRED,       /* freed but not yet returned to OS */
} page_state_t;

struct page_meta_s {
    /* Owning arena & segment */
    arena_t    *arena;
    segment_t  *segment;
    _Atomic(uint32_t) owner_tid;  /* thread ID of the owning thread */

    /* Slab parameters */
    uint8_t     bin_idx;        /* size class bin index */
    uint8_t     state;          /* page_state_t */
    uint32_t    slot_size;      /* size of each slot in bytes */
    uint16_t    capacity;       /* total slots in page */
    _Atomic(uint16_t) used;     /* slots currently allocated */

    /* Bump allocator: next virgin slot offset from page start */
    uint32_t    bump_offset;    /* byte offset for next bump alloc */
    uint32_t    bump_end;       /* end of bumpable region */

    /* Local free list (owning thread only, no atomics needed) */
    void       *local_free;     /* singly-linked free list */
    uint16_t    local_free_count;

    /* Remote free list (cross-thread, lock-free Treiber stack) */
    tagged_ptr_t remote_free;
    _Atomic(uint16_t) remote_free_count;

    /* Doubly-linked list for arena bin pages */
    page_meta_t *next;
    page_meta_t *prev;

    /* Page index within segment */
    uint16_t    page_index;
    uint16_t    _pad;
};

/* ──────────────────────────────────────────────
 * Segment: 4 MB mmap region
 * Page 0 = this header + page_meta array
 * Pages 1..63 = data pages
 * ────────────────────────────────────────────── */
struct segment_s {
    uint32_t        magic;          /* 0xA110CA7E */
    uint16_t        page_count;     /* number of data pages in use */
    uint16_t        pages_used;     /* pages currently allocated */
    arena_t        *arena;          /* owning arena */

    /* Linked list of segments in arena */
    segment_t      *next;
    segment_t      *prev;

    /* Page metadata array (embedded in page 0) */
    page_meta_t     pages[PAGES_PER_SEGMENT];
};

#define SEGMENT_MAGIC 0xA110CA7E

/* ──────────────────────────────────────────────
 * Arena: per-thread (or shared) allocation context
 * ────────────────────────────────────────────── */
struct arena_s {
    pthread_mutex_t lock;
    uint32_t        id;
    _Atomic(int)    thread_count;   /* threads attached to this arena */

    /* Per-bin page lists: head of doubly-linked list of pages with free slots */
    page_meta_t    *bins[BIN_COUNT];

    /* Segments owned by this arena */
    segment_t      *segments;       /* linked list head */
    uint32_t        segment_count;

    /* Statistics (relaxed atomics — updated on every alloc/free) */
    _Atomic(size_t) allocated;      /* total bytes given to callers */
    _Atomic(size_t) freed;          /* total bytes returned by callers */
    _Atomic(size_t) alloc_count;    /* total allocation count */
    _Atomic(size_t) free_count;     /* total free count */
};

/* ──────────────────────────────────────────────
 * Thread-local data
 * ────────────────────────────────────────────── */
struct tld_s {
    arena_t *arena;
    uint32_t thread_id;
    /* Per-bin page cache: the thread's "current" page for each bin.
     * Alloc can try this page first without taking the arena lock. */
    page_meta_t *bin_page[BIN_COUNT];

    /* Thread-local statistics (flushed to arena periodically and on exit) */
    size_t stat_allocated;      /* bytes allocated since last flush */
    size_t stat_freed;          /* bytes freed since last flush */
    size_t stat_alloc_count;    /* allocs since last flush */
    size_t stat_free_count;     /* frees since last flush */
    uint32_t stat_ops;          /* ops since last flush (triggers flush) */
};

/* ──────────────────────────────────────────────
 * Large allocation: direct mmap, tracked globally
 * ────────────────────────────────────────────── */
struct large_alloc_s {
    void           *base;       /* mmap base (may differ from user ptr for alignment) */
    size_t          size;       /* total mmap size */
    size_t          user_size;  /* requested size */
    large_alloc_t  *next;
    large_alloc_t  *prev;
};

/* ──────────────────────────────────────────────
 * Global state
 * ────────────────────────────────────────────── */
typedef struct {
    _Atomic(int)    initialized;
    pthread_mutex_t init_lock;

    /* Arena pool */
    arena_t         arenas[MAX_ARENAS];
    _Atomic(int)    arena_count;
    _Atomic(int)    arena_next;     /* round-robin counter */

    /* Large allocations list */
    large_alloc_t  *large_list;
    pthread_mutex_t large_lock;

    /* Bootstrap buffer for pre-init allocations */
    char            bootstrap_buf[BOOTSTRAP_BUF_SIZE];
    _Atomic(size_t) bootstrap_used;

    /* Large allocation statistics */
    _Atomic(size_t) large_allocated;    /* bytes currently in large allocs */
    _Atomic(size_t) large_alloc_count;  /* active large allocation count */

    /* OS memory tracking */
    _Atomic(size_t) mmap_bytes;         /* total bytes currently mmap'd */
    _Atomic(size_t) peak_in_use;        /* high watermark of bytes in use */

    /* Segment registry: hash set of active segment base addresses */
    _Atomic(uintptr_t) segment_registry[SEGMENT_REGISTRY_SIZE];
    pthread_mutex_t     segment_registry_lock;

    /* Size class tables (initialized once) */
    uint32_t        bin_size[BIN_COUNT];    /* bin index → slot size */
    uint8_t         size_to_bin_small[SMALL_MAX / 8 + 1]; /* quick lookup for <=128 */
} global_state_t;

extern global_state_t g_state;

/* ──────────────────────────────────────────────
 * Function declarations
 * ────────────────────────────────────────────── */

/* os.c */
void   *os_mmap_aligned(size_t size, size_t alignment);
void    os_munmap(void *addr, size_t size);
void    os_madvise_free(void *addr, size_t size);
size_t  os_page_size(void);

/* size_class.c */
void    size_class_init(void);
uint8_t size_to_bin(size_t size);
size_t  bin_to_size(uint8_t bin);

/* segment.c */
segment_t  *segment_create(arena_t *arena);
void        segment_destroy(segment_t *seg);
page_meta_t *segment_alloc_page(segment_t *seg, uint8_t bin_idx);
void        segment_free_page(segment_t *seg, page_meta_t *page);
segment_t  *ptr_to_segment(const void *ptr);
page_meta_t *ptr_to_page(const void *ptr);
void       *page_start(page_meta_t *page);

/* page.c */
void    page_init(page_meta_t *page, segment_t *seg, uint16_t page_index,
                  uint8_t bin_idx, uint32_t slot_size);
void   *page_alloc_slot(page_meta_t *page);
void    page_free_slot_local(page_meta_t *page, void *ptr);
void    page_free_slot_remote(page_meta_t *page, void *ptr);
void    page_collect_remote(page_meta_t *page);
bool    page_is_empty(page_meta_t *page);
void    page_retire(page_meta_t *page);

/* slab.c */
void   *slab_alloc(size_t size);
void   *slab_alloc_zeroed(size_t size, bool *zeroed);
void    slab_free(void *ptr);

/* arena.c */
void    arena_init(arena_t *arena, uint32_t id);
void   *arena_alloc(arena_t *arena, size_t size, bool *zeroed);
void    arena_free(arena_t *arena, void *ptr, page_meta_t *page);
arena_t *arena_get(void);

/* tld.c */
void    tld_init(void);
tld_t  *tld_get(void);
void    tld_cleanup(void *arg);

/* large.c */
void   *large_alloc(size_t size);
void   *large_alloc_aligned(size_t size, size_t alignment);
void    large_free(void *ptr);
large_alloc_t *large_find(const void *ptr);
size_t  large_usable_size(const void *ptr);

/* init.c */
void    malloc_init(void);
void    malloc_ensure_init(void);
bool    malloc_is_initialized(void);
void   *bootstrap_alloc(size_t size);

/* malloc.c — public API */
void   *my_malloc(size_t size);
void    my_free(void *ptr);
void   *my_calloc(size_t count, size_t size);
void   *my_realloc(void *ptr, size_t new_size);
void   *my_memalign(size_t alignment, size_t size);
int     my_posix_memalign(void **memptr, size_t alignment, size_t size);
void   *my_valloc(size_t size);
size_t  my_malloc_usable_size(const void *ptr);

/* zone.c (macOS) */
#ifndef MALLOC_NO_ZONE
void    zone_register(void);
#endif

#endif /* MALLOC_TYPES_H */
