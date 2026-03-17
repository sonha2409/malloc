#ifndef MALLOC_CONFIG_H
#define MALLOC_CONFIG_H

#include <stddef.h>

/* Segment: 4 MB aligned mmap region */
#define SEGMENT_SHIFT       22
#define SEGMENT_SIZE        ((size_t)1 << SEGMENT_SHIFT)       /* 4 MB */
#define SEGMENT_MASK        (~(SEGMENT_SIZE - 1))

/* Page: 64 KB logical page within a segment */
#define PAGE_SHIFT          16
#define PAGE_SIZE_ALLOC     ((size_t)1 << PAGE_SHIFT)          /* 64 KB */
#define PAGE_MASK           (~(PAGE_SIZE_ALLOC - 1))

/* Number of pages per segment (first page reserved for metadata) */
#define PAGES_PER_SEGMENT   (SEGMENT_SIZE / PAGE_SIZE_ALLOC)   /* 64 */
#define PAGES_DATA_START    1  /* page 0 is the segment header */

/* Size class boundaries */
#define SMALL_MAX           128
#define MEDIUM_MAX          (128 * 1024)  /* 128 KB */
#define LARGE_THRESHOLD     MEDIUM_MAX

/* Size class bins */
/* 8-byte spacing for <=128B: bins 0..15 (16 bins, sizes 8,16,...,128) */
/* ~12.5% spacing for 128B–128KB: ~59 bins */
#define SMALL_BIN_COUNT     16
#define BIN_COUNT           75

/* Arena count (fixed pool) */
#define MAX_ARENAS          64

/* Segment registry (hash set of active segment base addresses) */
#define SEGMENT_REGISTRY_SIZE  256  /* power of 2, max active segments */

/* Alignment */
#define MIN_ALIGN           16   /* minimum allocation alignment */

/* Debug mode */
#ifndef MALLOC_DEBUG
#define MALLOC_DEBUG        0
#endif

/* Bootstrap buffer for pre-init allocations */
#define BOOTSTRAP_BUF_SIZE  (64 * 1024)

#endif /* MALLOC_CONFIG_H */
