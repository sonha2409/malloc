# Custom Memory Allocator (malloc)

## Project Overview

A production-grade custom `malloc` implementation for macOS, designed for real-world interposition via `DYLD_INSERT_LIBRARIES`. Inspired by mimalloc and DLMalloc. Written in C11.

## Architecture

**Hierarchy:** Global State → Arenas (4, per-thread) → Segments (4MB mmap) → Pages (64KB) → Slots (fixed-size slab)

- Small/medium allocations (≤32KB effective): slab allocator with 75 size-class bins
- Large allocations (>32KB or slot > PAGE_SIZE/2): direct `mmap`, tracked in global linked list
- macOS integration: full `malloc_zone_t` + symbol interposition in `src/zone.c`

## Key Documentation

- `DESIGN.md` — comprehensive architecture and design document (read this first for deep understanding)
- `SPEC.md` — original specification
- `Malloc.pdf` — reference material on allocator fundamentals

## Source Layout

```
include/
  malloc_config.h    — constants (SEGMENT_SIZE, PAGE_SIZE, BIN_COUNT, thresholds)
  malloc_types.h     — all structs + function declarations (everything depends on this)
  malloc_atomic.h    — tagged pointers, CAS wrappers, Treiber stack primitives
src/
  slab.c             — HOT PATH: fast alloc/free with TLD page cache (most perf-critical)
  page.c             — page lifecycle, bump alloc, local/remote free lists
  segment.c          — 4MB segment create/destroy, segment registry, ptr→metadata lookup
  arena.c            — per-arena page management, slow-path allocation (mutex)
  tld.c              — thread-local data via pthread_key, per-bin page cache
  large.c            — direct mmap alloc/free for large allocations
  malloc.c           — public API: my_malloc, my_free, my_calloc, my_realloc, my_memalign
  zone.c             — macOS malloc_zone_t callbacks + symbol interposition (malloc/free/etc.)
  init.c             — global initialization, bootstrap buffer
  os.c               — OS abstraction (mmap, munmap, madvise)
  size_class.c       — 75-bin size class tables and lookup
tests/
  test_basic.c       — basic alloc/free/calloc correctness
  test_realloc.c     — realloc edge cases
  test_large.c       — large allocation stress
  test_alignment.c   — memalign/posix_memalign/valloc
  test_threads.c     — 8-thread concurrent alloc/free + cross-thread free
  bench_throughput.c — performance benchmarking
```

## Build & Test

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release   # or Debug for MALLOC_DEBUG=1
make

# Run all tests (must all pass before any PR/commit)
./test_basic && ./test_realloc && ./test_large && ./test_alignment && ./test_threads

# Benchmark
./bench_throughput

# Test zone interposition with a real program
DYLD_INSERT_LIBRARIES=./libmalloc.dylib /bin/ls
```

## Critical Invariants — DO NOT VIOLATE

1. **`page->local_free` is single-writer.** Only the page owner thread (identified by `page->owner_tid`) may read or write `local_free`. No lock protects it. If two threads touch it, you get data corruption. The arena slow path (`arena_alloc`) must NEVER access `local_free` — it only uses bump allocation.

2. **Segments are never unmapped.** Threads cache `page_meta_t *` pointers in `tld->bin_page[]`, which point into the segment header (page 0). Unmapping a segment while any thread holds a stale cache pointer causes a segfault. To add segment reclamation, you must first invalidate all TLD caches.

3. **`ptr_to_segment()` uses the segment registry, not pointer dereference.** The old approach of `((segment_t*)(ptr & MASK))->magic` crashed on large-allocation pointers whose 4MB-aligned base was unmapped. The registry (hash set of active segment base addresses) is safe.

4. **Page ownership is claimed via CAS.** Only one thread can own a page. Use `atomic_compare_exchange_strong` on `owner_tid` — never unconditionally write it.

## Compile-Time Flags

| Define | Default | Effect |
|--------|---------|--------|
| `MALLOC_DEBUG` | 0 | Enable debug assertions (set by `-DCMAKE_BUILD_TYPE=Debug`) |
| `MALLOC_NO_ZONE` | 0 | Disable zone registration (used by static test library) |

## Testing Guidelines

- After ANY change to concurrency code (`slab.c`, `page.c`, `arena.c`, `tld.c`), run `test_threads` at least 10 times: `for i in $(seq 1 10); do ./test_threads || echo "FAIL $i"; done`
- For suspected data races, compile with `-fsanitize=thread` and test
- For memory errors, compile with `-fsanitize=address` and test
- All 5 test binaries must pass before considering any change complete

## User Interaction

- **Challenge unreliable approaches**: If the user gives instructions or approaches that are unreliable, error-prone, or violate the critical invariants above, **warn them and explain the concern before proceeding**. Suggest the recommended alternative. Only proceed with the original approach if the user explicitly confirms after being informed.
- **Approval gates**: Present the approach/design before writing code. Wait for explicit approval ("go ahead", "do it", etc.) before implementing. If requirements are ambiguous, ask — don't assume.
- **Debugging protocol**: Diagnose root cause first, present fix options with trade-offs, wait for approval before editing. After applying a fix, build and run tests to confirm no regressions.

## Workflow

- Implement **one phase at a time**. Stop after each phase is complete.
- After each phase, the user will manually test. Do not proceed to the next phase until told to.
- Once the user confirms the phase works, they will commit and push to GitHub.
- After each phase is committed and pushed, remind the user: **"This conversation's context is getting longer. Starting a new session is recommended so I load fresh with CLAUDE.md and memory — I won't lose any project knowledge since it's all in the repo and my memory files. Stay in this session only if the next phase is tightly coupled to what we just did."**
- **Project knowledge lives in the repo**: Store all project context in `CLAUDE.md`, `SPEC.md`, and `DESIGN.md` — not in `~/.claude/` memory files. These repo files are the single source of truth.

## Git Conventions

- **Commits**: Conventional Commits format — `feat:`, `fix:`, `chore:`, `refactor:`, `test:`
- **No Co-Authored-By**: Never include `Co-Authored-By` lines or any co-author attribution in commit messages.
- Don't commit secrets or `.env` files.
- Don't over-engineer. Build what's needed for the current phase, not hypothetical future phases.

## Current Performance (Apple Silicon, Release)

| Benchmark | Throughput |
|-----------|-----------|
| Small alloc+free (64B) | ~53M ops/sec |
| Varied sizes | ~42M ops/sec |
| Batch (1000 alloc then free) | ~63M ops/sec |
| 4-thread concurrent | ~212M ops/sec |
