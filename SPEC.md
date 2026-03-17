# Custom Memory Allocator — Feature Spec & Progress Log

> **Purpose**: Living document tracking all features, design decisions, and implementation progress. Each session should read this first to know where we left off.

## Design Overview

A production-grade memory allocator inspired by DLMalloc (PDF reference) and mimalloc, targeting macOS (ARM64) with `DYLD_INSERT_LIBRARIES` interposition.

### Core Architecture

```
Global State → Arenas (per-thread) → Segments (4MB mmap-aligned)
    → Pages (64KB, slab-style) → Slots (fixed-size per page)
```

### Key Design Decisions

| Area | Decision | Rationale |
|---|---|---|
| OS interface | `mmap()` not `sbrk()` | sbrk is deprecated on macOS; mmap is portable and supports alignment |
| Metadata | Out-of-band (in segment header) | Security: user buffer overflows can't corrupt allocator metadata |
| Ptr→metadata | Bitmask `ptr & ~(4MB-1)` → segment header | O(1) lookup, no hash table, no global data structure |
| Small/medium | Slab allocator (fixed slots per page) | No coalescing needed; eliminates boundary tags entirely |
| Page layout | Hybrid bump-then-free-list | Bump for virgin pages (fast, sequential), free list after first frees |
| Large (>128KB) | Direct `mmap`, tracked in global list | Avoids fragmenting the slab allocator with huge blocks |
| Size classes | ~75 bins: 8B spacing ≤128B, ~12.5% above | ≤12.5% internal fragmentation, matches mimalloc approach |
| Threading | Per-thread arenas | Eliminates lock contention on the hot path |
| Cross-thread free | Lock-free CAS Treiber stack | No mutex needed; owning thread drains atomically |
| Memory return | `MADV_FREE` on empty pages, `munmap` on empty segments | Lazy return under pressure; keeps virtual addresses for reuse |
| macOS interop | Full `malloc_zone_t` registration | Required for CoreFoundation/ObjC compatibility |
| Build system | Modular `src/` with CMake | Cross-platform, test integration, IDE support |

### Data Structures

**segment_t** (at base of each 4MB region):
- magic, page_count, owning arena, prev/next segment links
- `page_meta_t pages[64]` — page descriptor array (page 0 = header, 1-63 = usable)

**page_meta_t** (per-page, stored in segment header):
- kind, bin_index, slot_size, capacity, used_count
- bump_offset (for virgin allocation), free_list_offset (local free list head)
- `_Atomic remote_free_head` (tagged pointer, CAS-based cross-thread free list)
- prev/next in arena's bin page list

**arena_t**:
- per-arena mutex, bin_pages[75] (page lists per size class), segment list

**tld_t** (thread-local):
- assigned arena, active_page[75] (current bump page per size class)

**large_alloc_t**:
- base, size, user_size, prev/next in global list

### Allocation Fast Path (no locks, no atomics)
1. Check `tld->active_page[bin]`
2. Bump allocate if page has virgin slots
3. Pop from local free list if available
4. (Slower) Atomic-exchange drain remote free list
5. (Slowest) Lock arena, find/create page

### Free Fast Path
1. Bitmask ptr → segment → page_meta
2. If same arena as current thread: push to local free list (no lock)
3. If different arena: CAS push to remote free list (lock-free)

---

## Feature Log

### Phase 0: Bootstrap & OS Layer
Status: **DONE**

- [x] **F0.1**: CMakeLists.txt — build .dylib + test binaries (2026-03-16)
- [x] **F0.2**: `malloc_config.h` — all constants (SEGMENT_SIZE, PAGE_SIZE, LARGE_THRESHOLD, bin counts) (2026-03-16)
- [x] **F0.3**: `malloc_types.h` — segment_t, page_meta_t, arena_t, tld_t, large_alloc_t, enums (2026-03-16)
- [x] **F0.4**: `malloc_atomic.h` — tagged pointer make/extract, CAS wrappers, memory ordering (2026-03-16)
- [x] **F0.5**: `os.c` — os_mmap_aligned(size, align), os_munmap, os_madvise, os_page_size (2026-03-16)
- [x] **F0.6**: `size_class.c` — bin_to_size[75] table, size_to_bin() lookup, unit tests (2026-03-16)

### Phase 1: Single-Threaded Slab Allocator
Status: **DONE**

- [x] **F1.1**: `segment.c` — segment_create (4MB aligned mmap), segment header init, segment_alloc_page (2026-03-16)
- [x] **F1.2**: `page.c` — page_init, bump allocation, local free list (slot_index_t linked list in freed slot data) (2026-03-16)
- [x] **F1.3**: `slab.c` — slab_alloc (bump → free list → new page), slab_free (local push + used_count--) (2026-03-16)
- [x] **F1.4**: `arena.c` — single arena, bin_pages[75] doubly-linked lists, page lookup from arena (2026-03-16)
- [x] **F1.5**: `tld.c` — simple thread-local data, single arena assignment (2026-03-16)
- [x] **F1.6**: `init.c` — global state init, single arena creation (2026-03-16)
- [x] **F1.7**: `malloc.c` — malloc(), free(), calloc() entry points (2026-03-16)
- [x] **F1.8**: Basic test suite: alloc/free various sizes, no overlap, no corruption (2026-03-16)

### Phase 2: realloc, Large Allocations, Alignment
Status: **DONE**

- [x] **F2.1**: `large.c` — large_alloc (direct mmap), large_free (munmap), large_alloc_t linked list (2026-03-16)
- [x] **F2.2**: `malloc.c` realloc — NULL→malloc, 0→free, same-class noop, cross-class copy (2026-03-16)
- [x] **F2.3**: `malloc.c` memalign/posix_memalign/valloc/pvalloc — alignment-aware allocation (2026-03-16)
- [x] **F2.4**: Page retirement — empty page → MADV_FREE, return to segment free pool (2026-03-16)
- [x] **F2.5**: Segment retirement — all pages free → munmap entire 4MB (2026-03-16)
- [x] **F2.6**: malloc_usable_size() — return slot_size for slab, user_size for large (2026-03-16)
- [x] **F2.7**: Test suite: realloc edge cases, large allocs, alignment correctness (2026-03-16)

### Phase 3: Multi-Threading
Status: **DONE**

- [x] **F3.1**: `tld.c` — pthread_key-based TLD lifecycle, thread exit cleanup destructor (2026-03-16)
- [x] **F3.2**: `arena.c` — multiple arenas (min(ncpus, 8)), round-robin thread assignment (2026-03-16)
- [x] **F3.3**: `page.c` — remote free list: CAS push (in slab_free), atomic_exchange drain (in slab_alloc) (2026-03-16)
- [x] **F3.4**: `arena.c` — per-arena mutex for page-level operations (not slot-level) (2026-03-16)
- [x] **F3.5**: `large.c` — thread-safe large alloc tracking (global mutex) (2026-03-16)
- [x] **F3.6**: `slab.c` — local vs remote free detection (compare segment->arena with tld->arena) (2026-03-16)
- [x] **F3.7**: Test suite: N-thread stress, cross-thread free correctness, no data races (TSan) (2026-03-16)

### Phase 4: macOS malloc_zone_t Integration
Status: **DONE**

- [x] **F4.1**: `zone.c` — malloc_zone_t struct with all callbacks (size, malloc, free, realloc, calloc, memalign) (2026-03-16)
- [x] **F4.2**: `zone.c` — malloc_introspection_t (force_lock/unlock for fork, good_size, statistics) (2026-03-16)
- [x] **F4.3**: `zone.c` — constructor: register as default zone (unregister old, register ours first) (2026-03-16)
- [x] **F4.4**: `init.c` — bootstrap buffer (static 64KB bump allocator for pre-init mallocs) (2026-03-16)
- [x] **F4.5**: `zone.c` — zone_size: correctly identify our allocations vs system allocations (2026-03-16)
- [x] **F4.6**: Fork safety: lock all arenas in pre-fork, unlock/reinit in post-fork (2026-03-16)
- [x] **F4.7**: Test: DYLD_INSERT_LIBRARIES works with /bin/ls, simple ObjC program (2026-03-16)

### Phase 5: Optimization & Hardening
Status: **PARTIAL**

- [x] **F5.1**: Hot path optimization — slab_alloc fast path ≤15 instructions (2026-03-16)
- [x] **F5.2**: `__builtin_expect` on all branches in hot path (2026-03-16)
- [ ] **F5.3**: Calloc optimization — skip memset for bump-allocated pages (mmap guarantees zero) *(conservative memset used for now)*
- [ ] **F5.4**: pressure_relief zone callback — MADV_FREE all empty pages on memory pressure *(stub exists)*
- [ ] **F5.5**: Debug mode — heap integrity checker, double-free detection via per-page bitmap *(MALLOC_DEBUG flag exists but minimal)*
- [ ] **F5.6**: Statistics counters — total alloc/free/mmap bytes (atomic, low overhead) *(counters tracked per-arena, zone_statistics() not wired up)*

### Phase 6: Benchmarking & Tuning
Status: **PARTIAL**

- [x] **F6.1**: bench_throughput — single-thread alloc/free ops/sec vs system malloc (2026-03-16)
- [x] **F6.2**: bench_contention — multi-thread scaling (4-thread benchmark) (2026-03-16)
- [x] **F6.3**: Tune SEGMENT_SIZE, PAGE_SIZE, bin spacing based on profiling (2026-03-16)
- [ ] **F6.4**: Test with real programs: vim, nano, simple ObjC apps via DYLD_INSERT_LIBRARIES
- [ ] **F6.5**: Memory efficiency — measure RSS vs requested bytes under various workloads

---

## Session Log

| Date | Session | Work Done | Next Step |
|---|---|---|---|
| 2026-03-16 | #3 | Updated CLAUDE.md (workflow rules, git conventions, user interaction). Updated SPEC.md to reflect all implemented phases. | Finish Phase 5 (F5.3–F5.6) or Phase 6 (F6.4–F6.5) |
| 2026-03-16 | #2 | Implemented Phases 0–4 fully, Phase 5 partially (hot path + builtin_expect), Phase 6 partially (benchmarks). All 5 tests pass. Performance: ~53M small ops/sec, ~212M 4-thread ops/sec. | Finish Phase 5 remainders, Phase 6 real-program testing |
| 2026-03-16 | #1 | Design interview, spec creation | Begin Phase 0 (F0.1–F0.6) |
