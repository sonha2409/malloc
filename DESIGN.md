# Custom Memory Allocator: Design Document

## 1. Overview

This document describes the architecture, design rationale, and implementation details of a production-grade, general-purpose memory allocator targeting macOS (arm64/x86_64). The allocator is designed for real-world interposition via `DYLD_INSERT_LIBRARIES`, replacing the system `malloc` transparently for any program.

The design draws inspiration from modern allocators such as [mimalloc](https://github.com/microsoft/mimalloc) and [DLMalloc](http://gee.cs.oswego.edu/dl/html/malloc.html), combining slab-style fixed-size allocation with per-thread arenas, lock-free cross-thread frees, and full macOS `malloc_zone_t` integration.

### 1.1 Design Goals

| Goal | Approach |
|------|----------|
| **High throughput** | Lock-free fast path (~15 instructions for alloc + free) |
| **Thread scalability** | Per-thread page ownership; no shared locks on the hot path |
| **Low fragmentation** | Segregated size classes with slab allocation (no splitting/coalescing) |
| **macOS compatibility** | Full `malloc_zone_t` zone with symbol interposition |
| **Simplicity** | Out-of-band metadata, no inline headers, bitmask-based pointer lookup |
| **Memory return** | `MADV_FREE` on empty pages, `munmap` on empty segments |

### 1.2 Key Metrics (Apple Silicon, Release Build)

| Benchmark | Throughput |
|-----------|-----------|
| Single-thread alloc+free (64B) | ~53 million ops/sec |
| Varied sizes alloc+free | ~42 million ops/sec |
| Batch alloc+free (1000) | ~63 million ops/sec |
| 4-thread concurrent | ~212 million ops/sec |

---

## 2. Architecture

### 2.1 Memory Hierarchy

The allocator organizes memory into four levels:

```
Global State
  |
  +-- Arena 0 (threads 0, 4, 8, ...)
  |     +-- Segment A (4 MB mmap, aligned)
  |     |     +-- Page 0: segment header + page_meta_t[64]
  |     |     +-- Page 1: slab for bin 3 (32-byte slots)
  |     |     +-- Page 2: slab for bin 7 (64-byte slots)
  |     |     +-- ...
  |     |     +-- Page 63: slab for bin 12 (112-byte slots)
  |     +-- Segment B (4 MB mmap, aligned)
  |           +-- ...
  +-- Arena 1 (threads 1, 5, 9, ...)
  |     +-- ...
  +-- Arena 2 ...
  +-- Arena 3 ...
  |
  +-- Large allocation list (direct mmap, globally tracked)
```

| Level | Size | Count | Purpose |
|-------|------|-------|---------|
| **Global State** | ~100 KB | 1 | Arena pool, size class tables, segment registry, large list |
| **Arena** | ~6 KB | 4 (default) | Per-thread-group allocation context with bin page lists |
| **Segment** | 4 MB | Dynamic | Aligned mmap region; page 0 = metadata, pages 1-63 = data |
| **Page** | 64 KB | 63 per segment | Slab of fixed-size slots for a single size class |
| **Slot** | 8B-32KB | varies | Individual user allocation (no inline header) |

### 2.2 Allocation Categories

| Category | Size Range | Strategy |
|----------|-----------|----------|
| Small | 1 - 128 bytes | Slab: 8-byte-spaced size classes (16 bins) |
| Medium | 129 bytes - 32 KB | Slab: ~12.5% geometric spacing (~59 bins) |
| Large (slab) | 32 KB - 128 KB | Routed to large alloc if slot > PAGE_SIZE/2 |
| Large (direct) | > 128 KB | Direct `mmap` with header, globally tracked |

---

## 3. Data Structures

### 3.1 Segment (`segment_t`)

A segment is a 4 MB region obtained via `mmap`, aligned to a 4 MB boundary. The alignment enables constant-time pointer-to-segment lookup via bitmask:

```c
segment_t *seg = (segment_t *)(ptr & ~(4MB - 1));
```

**Layout:**

```
Offset 0x000000 - 0x00FFFF  (64 KB):  Segment header (segment_t)
  - magic (0xA110CA7E), arena pointer, segment linked list
  - page_meta_t pages[64]   (96 bytes each = 6144 bytes total)

Offset 0x010000 - 0x01FFFF  (64 KB):  Data page 1
Offset 0x020000 - 0x02FFFF  (64 KB):  Data page 2
...
Offset 0x3F0000 - 0x3FFFFF  (64 KB):  Data page 63
```

A **segment registry** (global hash set of active segment base addresses) ensures that `ptr_to_segment()` never dereferences unmapped memory. The registry uses open-addressing with atomic CAS for lock-free insertion and lookup.

### 3.2 Page Metadata (`page_meta_t`)

Each page's metadata lives out-of-band in the segment header (page 0), not inline with user data. This eliminates the need for per-allocation headers and improves cache locality of user data.

```c
struct page_meta_s {
    arena_t         *arena;             // owning arena
    segment_t       *segment;           // parent segment
    _Atomic(uint32_t) owner_tid;        // owning thread ID (for fast-path free)

    uint8_t          bin_idx;           // size class index
    uint8_t          state;             // UNUSED | ACTIVE | FULL | RETIRED
    uint32_t         slot_size;         // bytes per slot
    uint16_t         capacity;          // total slots in page
    _Atomic(uint16_t) used;             // slots currently allocated

    uint32_t         bump_offset;       // next virgin slot offset
    uint32_t         bump_end;          // end of bumpable region

    void            *local_free;        // thread-local free list (singly-linked)
    uint16_t         local_free_count;

    tagged_ptr_t     remote_free;       // lock-free Treiber stack (cross-thread)
    _Atomic(uint16_t) remote_free_count;

    page_meta_t     *next, *prev;       // arena bin list (doubly-linked)
    uint16_t         page_index;        // index within segment
};
```

**State machine:**

```
UNUSED --[segment_alloc_page]--> ACTIVE --[all slots used]--> FULL
   ^                                |                           |
   |                                |   [remote frees arrive]   |
   +---[page_is_empty, retire]------+<--------------------------+
```

### 3.3 Arena (`arena_t`)

An arena groups pages by size class and protects shared state with a mutex. The mutex is only acquired on the **slow path** (new page allocation); the fast path is entirely lock-free.

```c
struct arena_s {
    pthread_mutex_t  lock;
    uint32_t         id;
    _Atomic(int)     thread_count;

    page_meta_t     *bins[75];          // per-bin doubly-linked page lists
    segment_t       *segments;          // owned segments list
    uint32_t         segment_count;

    _Atomic(size_t)  allocated;         // statistics
    _Atomic(size_t)  freed;
};
```

Threads are assigned to arenas round-robin. With 4 arenas, contention is reduced by 4x compared to a single global lock.

### 3.4 Thread-Local Data (`tld_t`)

Each thread maintains a TLD (via `pthread_key_t`) that caches the most recently used page for each size class bin:

```c
struct tld_s {
    arena_t      *arena;                // assigned arena
    uint32_t      thread_id;            // unique thread ID
    page_meta_t  *bin_page[75];         // per-bin cached page pointer
};
```

The `bin_page` cache is the key to the fast path: if the cached page has slots on its local free list or bump space, allocation completes without any lock or atomic operation.

### 3.5 Large Allocation (`large_alloc_t`)

Allocations exceeding the slab threshold are served directly by `mmap`. A small header is placed at the start of the mapping:

```c
struct large_alloc_s {
    void            *base;              // mmap base address
    size_t           size;              // total mmap size
    size_t           user_size;         // requested size
    large_alloc_t   *next, *prev;       // global doubly-linked list
};
```

The global list is protected by `g_state.large_lock`.

---

## 4. Size Classes

The allocator uses 75 size class bins to minimize internal fragmentation:

**Small bins (0-15):** Linear 8-byte spacing.

| Bin | Size | Bin | Size | Bin | Size | Bin | Size |
|-----|------|-----|------|-----|------|-----|------|
| 0 | 8 | 4 | 40 | 8 | 72 | 12 | 104 |
| 1 | 16 | 5 | 48 | 9 | 80 | 13 | 112 |
| 2 | 24 | 6 | 56 | 10 | 88 | 14 | 120 |
| 3 | 32 | 7 | 64 | 11 | 96 | 15 | 128 |

**Medium bins (16-74):** ~12.5% geometric growth from 144 bytes to 128 KB, rounded to 16-byte alignment. This yields at most 12.5% internal fragmentation for any allocation.

**Lookup:**
- For sizes <= 128 bytes: direct table lookup `size_to_bin_small[size / 8]` (O(1))
- For sizes > 128 bytes: binary search over 59 medium bins (O(log n), n=59)

---

## 5. Allocation & Free Algorithms

### 5.1 Allocation (`slab_alloc`)

The allocation path is designed for minimal instruction count on the common case:

```
slab_alloc(size):
  1. bin = size_to_bin(size)
  2. If size > PAGE_SIZE/2: goto large_alloc

  FAST PATH (no lock, no atomics):
  3. tld = pthread_getspecific(tld_key)
  4. page = tld->bin_page[bin]
  5. If page != NULL and page->state == ACTIVE:
     a. If page->local_free != NULL:
        - slot = page->local_free
        - page->local_free = *(void**)slot       // pop free list
        - page->used++
        - return slot                             // ~8 instructions
     b. If page->bump_offset < page->bump_end:
        - slot = page_start(page) + bump_offset
        - page->bump_offset += slot_size          // bump pointer
        - page->used++
        - return slot
     c. Collect remote frees (atomic exchange):
        - list = treiber_collect(&page->remote_free)
        - Prepend to local_free
        - Retry step 5a
     d. Page is full; clear cache: tld->bin_page[bin] = NULL

  SLOW PATH (arena mutex):
  6. arena = tld->arena
  7. Lock arena->lock
  8. Scan arena->bins[bin] for UNOWNED page with bump space
     (skip pages with owner_tid != UINT32_MAX to avoid racing
      with the owner's lock-free bump_offset access)
  9. If none: allocate new page from existing segment or new segment
  10. Bump-allocate from new/unowned page
  11. Unlock
  12. CAS page->owner_tid to claim ownership
  13. If claimed: tld->bin_page[bin] = page
  14. return slot
```

### 5.2 Free (`slab_free`)

```
slab_free(ptr):
  1. seg = ptr & ~(4MB-1)
  2. If seg not in segment_registry: goto large_free
  3. page = &seg->pages[offset / 64KB]

  FAST PATH (owning thread, no lock):
  4. If tld->thread_id == page->owner_tid:
     - *(void**)ptr = page->local_free
     - page->local_free = ptr                     // push to local free list
     - page->used--
     - return                                     // ~10 instructions

  SLOW PATH (remote thread, lock-free):
  5. treiber_push(&page->remote_free, ptr)        // atomic CAS loop
     - page->used--
     - return
```

### 5.3 Large Allocation

```
large_alloc(size):
  1. total = round_up(LARGE_HEADER_SIZE + size, os_page_size)
  2. base = mmap(total)
  3. Store large_alloc_t header at base
  4. Lock g_state.large_lock
  5. Prepend to global large_list
  6. Unlock
  7. return base + LARGE_HEADER_SIZE

large_free(ptr):
  1. Lock g_state.large_lock
  2. Walk large_list, find entry containing ptr
  3. Unlink from list
  4. Unlock
  5. munmap(base, size)
```

---

## 6. Threading Model

### 6.1 Ownership and Contention Avoidance

The core insight: if the same thread that allocated a slot also frees it (the common case), **no synchronization is needed**. This is achieved through page ownership:

1. When a thread first allocates from a page, it atomically claims ownership via CAS on `page->owner_tid`.
2. The owning thread accesses `page->local_free` without any lock or atomic.
3. Other threads freeing to this page use the atomic `remote_free` Treiber stack.
4. The owner periodically collects remote frees into the local list (during allocation, when the local list is empty).

**Contention points:**

| Operation | Synchronization | Frequency |
|-----------|----------------|-----------|
| Fast alloc (TLD cache hit) | None | ~95% of allocs |
| Fast free (owner thread) | None | ~80-90% of frees |
| Remote free (cross-thread) | Lock-free CAS | ~10-20% of frees |
| Slow alloc (new page) | Arena mutex | ~5% of allocs |
| Large alloc/free | Global mutex | Rare |

### 6.2 Treiber Stack (Lock-Free Remote Free List)

Cross-thread frees use a Treiber stack with tagged pointers to prevent the ABA problem:

```c
typedef struct {
    _Atomic(uint64_t) value;   // [16-bit tag | 48-bit pointer]
} tagged_ptr_t;
```

- **Push** (free): CAS loop to prepend node to stack head, incrementing tag.
- **Collect** (alloc): Atomic exchange of head with NULL, returning entire list.

The tag uses the upper 16 bits of the 64-bit value (arm64/x86_64 use 48-bit virtual addresses), providing 65536 unique tags before wrap-around, making ABA practically impossible.

### 6.3 Arena Assignment

Threads are assigned to arenas via atomic round-robin:

```c
int idx = atomic_fetch_add(&arena_next, 1) % arena_count;
```

With 4 arenas, threads 0/4/8/... share arena 0, threads 1/5/9/... share arena 1, etc. This reduces mutex contention on the slow path by a factor of `arena_count`.

### 6.4 Fork Safety

The `malloc_introspection_t` provides `force_lock` and `force_unlock` callbacks that macOS invokes around `fork()`:

- `force_lock`: Acquires all arena mutexes and the large allocation mutex.
- `force_unlock`: Releases all mutexes.

This prevents deadlocks in the child process after `fork()`.

---

## 7. macOS Integration

### 7.1 `malloc_zone_t` Registration

The allocator registers a full `malloc_zone_t` with the macOS malloc subsystem:

```c
static malloc_zone_t custom_zone = {
    .size               = zone_size,
    .malloc             = zone_malloc,
    .calloc             = zone_calloc,
    .valloc             = zone_valloc,
    .free               = zone_free,
    .realloc            = zone_realloc,
    .memalign           = zone_memalign,
    .batch_malloc       = zone_batch_malloc,
    .batch_free         = zone_batch_free,
    .introspect         = &zone_introspection,
    .claimed_address    = zone_claimed_address,
    .pressure_relief    = zone_pressure_relief,
    .version            = 12,
    .zone_name          = "custom_malloc",
    // ...
};
```

A `__attribute__((constructor))` function registers the zone at load time, before `main()`.

### 7.2 Symbol Interposition

For `DYLD_INSERT_LIBRARIES` support, the shared library directly defines `malloc`, `free`, `calloc`, `realloc`, `valloc`, `posix_memalign`, `malloc_size`, `malloc_good_size`, and `malloc_usable_size`. These symbols override the system implementations when the library is loaded first.

### 7.3 `claimed_address`

The zone's `claimed_address` callback is critical for multi-zone compatibility. It returns true if a pointer belongs to our allocator by checking:

1. The bootstrap buffer range
2. The segment registry (slab allocations)
3. The large allocation list

### 7.4 Bootstrap Buffer

During early process initialization (before `malloc_init` completes), the allocator serves allocations from a static 64 KB bootstrap buffer using a simple bump allocator. This avoids recursion when `pthread_key_create` or other initialization functions call `malloc`.

---

## 8. Memory Management

### 8.1 OS Interface

| Function | System Call | Purpose |
|----------|-----------|---------|
| `os_mmap_aligned(size, align)` | `mmap` + trim | Allocate aligned virtual memory |
| `os_munmap(addr, size)` | `munmap` | Release virtual memory |
| `os_madvise_free(addr, size)` | `madvise(MADV_FREE)` | Hint: pages can be reclaimed |

`MADV_FREE` (macOS) is preferred over `MADV_DONTNEED` because it allows the kernel to reclaim pages lazily, avoiding the cost of re-zeroing if the pages are reused before reclamation.

### 8.2 Segment Lifecycle

```
os_mmap_aligned(4MB, 4MB) --> ACTIVE
  |
  [pages allocated and freed]
  |
  [all pages UNUSED] --> Retained for reuse (not unmapped)
```

Segments are intentionally **not destroyed** even when all their pages are empty. This avoids a use-after-free hazard: other threads may hold TLD cache pointers (`bin_page[]`) to page metadata within the segment header. Since the header lives in the mmap region, unmapping it would cause those threads to segfault on their next allocation.

### 8.3 Page Retirement

When a page becomes empty (`used == 0` after collecting all remote frees):

1. The page is removed from the arena's bin list.
2. `madvise(MADV_FREE)` is called on the 64 KB data region.
3. The page metadata is reset to `PAGE_UNUSED`.
4. The page slot becomes available for reuse by any size class.

### 8.4 `realloc` Optimizations

| Case | Action |
|------|--------|
| `realloc(NULL, size)` | Equivalent to `malloc(size)` |
| `realloc(ptr, 0)` | Equivalent to `free(ptr)`, returns NULL |
| Same size class | Return `ptr` unchanged (no-op) |
| Different size class | `malloc(new)` + `memcpy` + `free(old)` |
| Large, fits in mmap | Update `user_size`, return `ptr` |
| Large, doesn't fit | `malloc(new)` + `memcpy` + `free(old)` |

---

## 9. Pointer Classification

Given an arbitrary pointer `ptr`, the allocator determines its origin in constant time:

```
ptr_to_segment(ptr):
  base = ptr & ~(4MB - 1)                          // bitmask
  if segment_registry_lookup(base): return base     // hash set O(1)
  else: return NULL                                 // not a slab allocation

ptr_to_page(ptr):
  seg = ptr_to_segment(ptr)
  page_idx = (ptr - seg) / 64KB                    // arithmetic
  return &seg->pages[page_idx]
```

The **segment registry** is a global open-addressing hash set of segment base addresses. It uses atomic CAS for insertion and atomic loads for lookup, making it lock-free for reads. This replaces the naive approach of dereferencing the aligned base address and checking a magic number, which would crash on pointers from large allocations or other memory regions.

---

## 10. Build System

### 10.1 Targets

```cmake
custommalloc        # Shared library (.dylib) with zone integration
custommalloc_static # Static library with MALLOC_NO_ZONE=1 (for tests)
test_basic          # Phase 1: basic alloc/free/calloc
test_realloc        # Phase 2: realloc edge cases
test_large          # Phase 2: large allocations
test_alignment      # Phase 2: memalign/posix_memalign/valloc
test_threads        # Phase 3: concurrent alloc/free, cross-thread free
bench_throughput    # Phase 6: performance benchmarking
```

### 10.2 Build & Test

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make

# Run all tests
./test_basic && ./test_realloc && ./test_large && ./test_alignment && ./test_threads

# Benchmark
./bench_throughput

# Interpose on a real program
DYLD_INSERT_LIBRARIES=./libmalloc.dylib /bin/ls
```

### 10.3 Compile-Time Configuration

| Define | Default | Purpose |
|--------|---------|---------|
| `MALLOC_DEBUG` | 0 | Enable debug assertions |
| `MALLOC_NO_ZONE` | 0 | Disable zone registration (for static test library) |

---

## 11. Source File Map

```
malloc/
  include/
    malloc_config.h      Constants and tuning parameters
    malloc_types.h       All data structures and function declarations
    malloc_atomic.h      Tagged pointers and lock-free primitives
  src/
    os.c                 OS abstraction (mmap, munmap, madvise)
    size_class.c         Size class table initialization and lookup
    segment.c            4 MB segment lifecycle and pointer classification
    page.c               Page-level allocation (bump + free list)
    slab.c               HOT PATH: fast alloc/free with TLD caching
    arena.c              Per-arena page management (slow path)
    tld.c                Thread-local data (pthread_key)
    large.c              Large allocation via direct mmap
    init.c               Global initialization and bootstrap
    malloc.c             Public API (malloc/free/calloc/realloc/memalign)
    zone.c               macOS malloc_zone_t and symbol interposition
  tests/
    test_basic.c         Basic allocation correctness
    test_realloc.c       realloc semantics
    test_large.c         Large allocation stress
    test_alignment.c     Alignment guarantees
    test_threads.c       Multi-threaded correctness
    bench_throughput.c   Performance benchmarking
```

---

## 12. Comparison with System Allocators

| Feature | This Allocator | macOS libmalloc | glibc malloc | mimalloc |
|---------|---------------|----------------|-------------|----------|
| Metadata | Out-of-band | Mixed | Inline headers | Out-of-band |
| Ptr lookup | Bitmask + registry | Zone dispatch | Chunk headers | Bitmask |
| Thread model | Per-thread pages | Magazine per-CPU | Arenas | Per-thread pages |
| Free list | Slab local + remote | Magazine | Bins + unsorted | Local + remote |
| Cross-thread | Lock-free Treiber | Depot (locked) | Fastbin lock | Lock-free |
| Size classes | 75 bins | ~50 tiny/small | ~128 bins | ~75 bins |
| Coalescing | None (slab) | None (slab) | Boundary tags | None (slab) |
| Large alloc | Direct mmap | Direct mmap | mmap threshold | Direct mmap |

---

## 13. Known Limitations and Future Work

### Current Limitations

1. **Segment retention**: Segments are never returned to the OS. Long-running programs with memory spikes will retain peak virtual memory. A segment reclamation policy (with TLD cache invalidation) would address this.

2. **Large allocation lookup**: `large_find()` walks a linked list in O(n). For programs with many large allocations, a hash map or balanced tree would be faster.

3. **Fixed arena count**: The number of arenas is fixed at initialization (4). Dynamic arena creation based on active thread count would improve scalability.

4. **No debug heap**: The `MALLOC_DEBUG` flag is defined but not fully utilized. A debug mode with guard pages, double-free detection, and buffer overflow checking would aid development.

5. **No `malloc_zone_pressure_relief`**: The pressure relief callback is a no-op. It could scan arenas for empty pages and `MADV_FREE` them.

### Future Optimizations

- ~~**Calloc fast path**~~: *(Implemented)* `slab_alloc_zeroed()` tracks whether the slot came from bump allocation; `my_calloc` skips `memset` when the slot is already zero.
- **Prefetching**: `__builtin_prefetch` on the next free list node during allocation.
- **NUMA awareness**: Bind arenas to specific CPU cores for NUMA locality.
- **Secure allocator mode**: Randomize slot order within pages, guard pages between segments, zero-on-free for sensitive allocations.

---

## 14. References

1. D. Lea, "A Memory Allocator," 1996. (DLMalloc)
2. D. Leijen, B. Zorn, L. de Moura, "Mimalloc: Free List Sharding in Action," Microsoft Research, 2019.
3. Apple, "malloc_zone_t," macOS Developer Documentation.
4. M. Michael, "Hazard Pointers: Safe Memory Reclamation for Lock-Free Objects," IEEE TPDS, 2004.
5. R. Treiber, "Systems Programming: Coping with Parallelism," IBM Research Report, 1986. (Treiber stack)
