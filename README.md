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

### Memory Efficiency

| Workload | Overhead (RSS vs requested) |
|---|---|
| Many small (100K × 64B) | +14% |
| Mixed sizes (16B–16KB) | +24% |
| Large (64KB–1MB) | +4% |
| Fragmentation stress | +59% |

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

Requires macOS, CMake 3.16+, and a C11 compiler (Clang/Xcode).

```bash
git clone https://github.com/sonha2409/malloc.git
cd malloc
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

This produces:
- `libmalloc.dylib` — shared library for interposition
- `test_basic`, `test_realloc`, `test_large`, `test_alignment`, `test_threads` — test suite
- `test_debug` — debug mode tests (double-free detection, heap integrity)
- `bench_throughput` — performance benchmark
- `bench_memory` — memory efficiency benchmark

### Build Modes

| Mode | Command | Effect |
|---|---|---|
| Release | `cmake .. -DCMAKE_BUILD_TYPE=Release` | `-O3`, no debug checks |
| Debug | `cmake .. -DCMAKE_BUILD_TYPE=Debug` | `-g -O0`, enables `MALLOC_DEBUG` assertions, double-free detection |

## Usage — Running Any Application with This Allocator

The allocator works as a **drop-in replacement** for the system `malloc`. You don't need to modify or recompile the target application. macOS's `DYLD_INSERT_LIBRARIES` mechanism loads our library before the system one, and our `malloc_zone_t` registration takes over all memory allocation.

### Quick Start

```bash
cd build

# Run any command with the custom allocator
DYLD_INSERT_LIBRARIES=./libmalloc.dylib <your-command-here>
```

### Examples with CLI Tools

```bash
# Basic commands
DYLD_INSERT_LIBRARIES=./libmalloc.dylib /bin/ls -la
DYLD_INSERT_LIBRARIES=./libmalloc.dylib /usr/bin/grep "pattern" file.txt
DYLD_INSERT_LIBRARIES=./libmalloc.dylib /usr/bin/sort < data.csv

# Pipe chains — each process in the pipeline uses the allocator
DYLD_INSERT_LIBRARIES=./libmalloc.dylib /bin/cat largefile.txt | \
  DYLD_INSERT_LIBRARIES=./libmalloc.dylib /usr/bin/sort | \
  DYLD_INSERT_LIBRARIES=./libmalloc.dylib /usr/bin/uniq -c
```

### Examples with Scripting Languages

```bash
# Python
DYLD_INSERT_LIBRARIES=./libmalloc.dylib python3 my_script.py

# Python one-liner
DYLD_INSERT_LIBRARIES=./libmalloc.dylib python3 -c "
import json
data = [{'id': i, 'value': 'x' * i} for i in range(10000)]
print(f'Created {len(data)} objects')
"

# Ruby
DYLD_INSERT_LIBRARIES=./libmalloc.dylib ruby my_script.rb

# Perl
DYLD_INSERT_LIBRARIES=./libmalloc.dylib perl my_script.pl
```

### Examples with GUI Applications

```bash
# VS Code
DYLD_INSERT_LIBRARIES=./libmalloc.dylib "/Applications/Visual Studio Code.app/Contents/MacOS/Electron"

# Firefox
DYLD_INSERT_LIBRARIES=./libmalloc.dylib /Applications/Firefox.app/Contents/MacOS/firefox

# Any third-party .app
DYLD_INSERT_LIBRARIES=./libmalloc.dylib /Applications/YourApp.app/Contents/MacOS/YourApp
```

> **Finding the executable inside a .app bundle:**
> ```bash
> # List the main executable
> ls /Applications/YourApp.app/Contents/MacOS/
> ```

### Examples with Servers and Long-Running Processes

```bash
# Node.js server
DYLD_INSERT_LIBRARIES=./libmalloc.dylib node server.js

# Go binary
DYLD_INSERT_LIBRARIES=./libmalloc.dylib ./my-go-server

# Any web server, database, or daemon
DYLD_INSERT_LIBRARIES=./libmalloc.dylib ./your-server --port 8080
```

### Using with Vim/Nano

```bash
# Open a file in vim with the custom allocator
DYLD_INSERT_LIBRARIES=./libmalloc.dylib vim myfile.txt

# Open a file in nano
DYLD_INSERT_LIBRARIES=./libmalloc.dylib nano myfile.txt
```

### Shell Alias (Use for Every Command)

Add to your `~/.zshrc` or `~/.bashrc` to make it easy to invoke:

```bash
# Point to wherever you built the library
export CUSTOM_MALLOC="/path/to/malloc/build/libmalloc.dylib"

# Alias for quick use
alias cmalloc='DYLD_INSERT_LIBRARIES=$CUSTOM_MALLOC'

# Then use it like:
# cmalloc python3 my_script.py
# cmalloc vim file.txt
# cmalloc /Applications/Firefox.app/Contents/MacOS/firefox
```

Or to use the custom allocator for **every command** in your shell session:

```bash
# Set for current terminal session (all subsequent commands use it)
export DYLD_INSERT_LIBRARIES=/path/to/malloc/build/libmalloc.dylib

# Now every command automatically uses the custom allocator
ls -la
python3 script.py
vim file.txt

# Unset to go back to system malloc
unset DYLD_INSERT_LIBRARIES
```

### Important: macOS System Integrity Protection (SIP)

macOS **blocks** `DYLD_INSERT_LIBRARIES` for certain binaries:

| Category | Examples | Works? |
|---|---|---|
| **Third-party apps** | VS Code, Firefox, Chrome, Slack, Spotify | Yes |
| **CLI tools in /usr/bin** | python3, ruby, perl, vim, grep, sort | Yes |
| **CLI tools in /bin** | ls, cat, sh, zsh | Yes |
| **Apple system apps** | Calculator, TextEdit, Safari, Notes | No (SIP blocks it) |
| **SIP-protected binaries** | Anything under `/System/` with Apple code signing | No |

If a program silently ignores the library or crashes with `Code Signature Invalid`, it's SIP — not an allocator bug. There is no workaround without disabling SIP (not recommended).

### Verifying the Allocator Is Active

To confirm the custom allocator is actually being used, you can check with a test program:

```c
// verify.c — compile with: clang -o verify verify.c
#include <malloc/malloc.h>
#include <stdio.h>

int main(void) {
    malloc_zone_t *zone = malloc_default_zone();
    printf("Default zone: %s\n", malloc_get_zone_name(zone));
    // Should print "custom" instead of "DefaultMallocZone"

    void *p = malloc(42);
    printf("malloc(42) = %p, usable size = %zu\n", p, malloc_size(p));
    free(p);
    return 0;
}
```

```bash
DYLD_INSERT_LIBRARIES=./libmalloc.dylib ./verify
# Output: Default zone: custom
```

### Direct API (Linking Directly)

If you want to use the allocator directly in your C project instead of interposing:

```c
#include "malloc_types.h"

void *p = my_malloc(256);
p = my_realloc(p, 512);
my_free(p);

void *aligned = my_memalign(64, 1024);  // 64-byte aligned
my_free(aligned);

void *zeroed = my_calloc(100, sizeof(int));  // zero-initialized
my_free(zeroed);

size_t usable = my_malloc_usable_size(p);  // actual slot size
```

Link against the static library:

```cmake
# In your CMakeLists.txt
target_link_libraries(your_target PRIVATE custommalloc_static pthread)
target_include_directories(your_target PRIVATE /path/to/malloc/include)
```

## Testing

```bash
cd build

# Run all correctness tests
./test_basic && ./test_realloc && ./test_large && ./test_alignment && ./test_threads

# Debug mode tests (double-free detection, heap integrity)
./test_debug

# Performance benchmark
./bench_throughput

# Memory efficiency benchmark
./bench_memory

# Real-program interposition test (15 programs)
bash ../tests/test_interpose.sh
```

### Test Suite

| Test | What It Covers |
|---|---|
| `test_basic` | Alloc, free, calloc, statistics correctness |
| `test_realloc` | Realloc edge cases (grow, shrink, NULL, zero) |
| `test_large` | Large allocation (>32KB) stress testing |
| `test_alignment` | `memalign`, `posix_memalign`, `valloc` |
| `test_threads` | 8-thread concurrent alloc/free, cross-thread free |
| `test_debug` | Double-free detection, allocation bitmap, heap integrity checker |
| `test_interpose.sh` | 15 real programs via `DYLD_INSERT_LIBRARIES` |

### Sanitizers

```bash
# Thread sanitizer (for data race detection)
cmake .. -DCMAKE_C_FLAGS="-fsanitize=thread"
make && ./test_threads

# Address sanitizer (for memory errors)
cmake .. -DCMAKE_C_FLAGS="-fsanitize=address"
make && ./test_basic
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
  debug.c            — debug mode: allocation bitmap, double-free detection
tests/
  test_*.c           — correctness tests
  test_interpose.sh  — real-program interposition tests
  bench_throughput.c — performance benchmarking
  bench_memory.c     — memory efficiency benchmarking
```

## Documentation

- **[DESIGN.md](DESIGN.md)** — comprehensive architecture document with data structures, algorithms, and design rationale
- **[SPEC.md](SPEC.md)** — feature specification and progress log

## License

This project is for educational and research purposes.
