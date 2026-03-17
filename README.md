# Custom Memory Allocator

A production-grade `malloc` implementation for macOS, written in C11. Designed for real-world interposition via `DYLD_INSERT_LIBRARIES`, transparently replacing the system allocator for any program.

Inspired by [mimalloc](https://github.com/microsoft/mimalloc) and [DLMalloc](http://gee.cs.oswego.edu/dl/html/malloc.html).

## Performance

Benchmarked on Apple Silicon (Release build):

| Benchmark | Throughput |
|---|---|
| Small alloc+free (64B) | **~53M ops/sec** |
| Varied sizes (16B–4KB) | **~42M ops/sec** |
| Batch (1000 alloc, then free) | **~63M ops/sec** |
| 4-thread concurrent | **~212M ops/sec** |

## Architecture

```
Global State
  ├── Arena 0 (threads 0, 4, 8, ...)
  │     ├── Segment (4 MB mmap, aligned)
  │     │     ├── Page 0: metadata (page_meta_t[64])
  │     │     ├── Page 1–63: 64 KB data pages (slab slots)
  │     │     └── ...
  │     └── Segment ...
  ├── Arena 1 (threads 1, 5, 9, ...)
  ├── Arena 2 ...
  ├── Arena 3 ...
  └── Large allocation list (direct mmap)
```

**Allocation strategies:**

| Category | Size Range | Strategy |
|---|---|---|
| Small | 1–128 B | Slab allocator, 8-byte-spaced size classes (16 bins) |
| Medium | 129 B – 32 KB | Slab allocator, ~12.5% geometric spacing (59 bins) |
| Large | > 32 KB | Direct `mmap`, tracked in a global linked list |

### Key Design Decisions

- **Out-of-band metadata** — page metadata lives in the segment header, not inline with user data. Buffer overflows can't corrupt allocator state.
- **Lock-free fast path** — allocation from a thread's cached page requires no locks or atomics (~8 instructions).
- **Cross-thread free via Treiber stack** — freeing memory owned by another thread uses a lock-free CAS push. The owning thread drains the stack lazily.
- **Per-thread arenas** — 4 arenas with round-robin assignment reduce contention by 4x.
- **75 size classes** — at most 12.5% internal fragmentation for any allocation.
- **Segment registry** — safe pointer-to-segment lookup via a global hash set, avoiding unsafe pointer dereferences.

## Building

Requires CMake 3.16+ and a C11 compiler.

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

This produces:
- `libmalloc.dylib` — shared library for interposition
- `test_basic`, `test_realloc`, `test_large`, `test_alignment`, `test_threads` — test suite
- `bench_throughput` — performance benchmark

### Build Modes

| Mode | Command | Effect |
|---|---|---|
| Release | `cmake .. -DCMAKE_BUILD_TYPE=Release` | `-O3`, no debug checks |
| Debug | `cmake .. -DCMAKE_BUILD_TYPE=Debug` | `-g -O0`, enables `MALLOC_DEBUG` assertions |

## Testing

```bash
cd build

# Run all tests
./test_basic && ./test_realloc && ./test_large && ./test_alignment && ./test_threads

# Run benchmark
./bench_throughput
```

### Test Suite

| Test | What It Covers |
|---|---|
| `test_basic` | Alloc, free, calloc correctness |
| `test_realloc` | Realloc edge cases (grow, shrink, NULL) |
| `test_large` | Large allocation stress testing |
| `test_alignment` | `memalign`, `posix_memalign`, `valloc` |
| `test_threads` | 8-thread concurrent alloc/free, cross-thread free |

### Sanitizers

```bash
# Thread sanitizer (for data race detection)
cmake .. -DCMAKE_C_FLAGS="-fsanitize=thread"
make && ./test_threads

# Address sanitizer (for memory errors)
cmake .. -DCMAKE_C_FLAGS="-fsanitize=address"
make && ./test_basic
```

## Usage

### As a drop-in system allocator replacement

```bash
DYLD_INSERT_LIBRARIES=./build/libmalloc.dylib /bin/ls
DYLD_INSERT_LIBRARIES=./build/libmalloc.dylib python3 -c "print('hello')"
```

The library registers a macOS `malloc_zone_t` and interposes the standard `malloc`/`free`/`calloc`/`realloc` symbols, so any program picks it up automatically.

### Direct API

```c
#include "malloc_types.h"

void *p = my_malloc(256);
p = my_realloc(p, 512);
my_free(p);

void *aligned = my_memalign(64, 1024);  // 64-byte aligned
my_free(aligned);
```

## Project Structure

```
include/
  malloc_config.h    — constants (segment size, page size, bin count)
  malloc_types.h     — all structs and function declarations
  malloc_atomic.h    — tagged pointers, CAS wrappers, Treiber stack
src/
  slab.c             — fast-path alloc/free (hot path)
  page.c             — page lifecycle, bump allocation, free lists
  segment.c          — 4MB segment management, segment registry
  arena.c            — per-arena page management (slow path)
  tld.c              — thread-local data (pthread_key)
  large.c            — direct mmap for large allocations
  malloc.c           — public API (my_malloc, my_free, etc.)
  zone.c             — macOS malloc_zone_t + symbol interposition
  init.c             — global initialization, bootstrap buffer
  os.c               — OS abstraction (mmap, munmap, madvise)
  size_class.c       — 75-bin size class tables and lookup
tests/
  test_*.c           — correctness tests
  bench_throughput.c — performance benchmarking
```

## Documentation

- **[DESIGN.md](DESIGN.md)** — comprehensive architecture document with data structures, algorithms, and design rationale
- **[SPEC.md](SPEC.md)** — feature specification and progress log
- **[Malloc.pdf](Malloc.pdf)** — reference material on allocator fundamentals

## License

This project is for educational and research purposes.
