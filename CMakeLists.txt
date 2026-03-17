cmake_minimum_required(VERSION 3.16)
project(custom_malloc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DMALLOC_DEBUG=1")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Source files
set(MALLOC_SOURCES
    src/os.c
    src/size_class.c
    src/segment.c
    src/page.c
    src/slab.c
    src/arena.c
    src/tld.c
    src/large.c
    src/init.c
    src/malloc.c
    src/zone.c
)

# Shared library (.dylib)
add_library(custommalloc SHARED ${MALLOC_SOURCES})
target_include_directories(custommalloc PUBLIC include)
target_link_libraries(custommalloc PRIVATE pthread)
set_target_properties(custommalloc PROPERTIES
    OUTPUT_NAME "malloc"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Static library for tests
add_library(custommalloc_static STATIC ${MALLOC_SOURCES})
target_include_directories(custommalloc_static PUBLIC include)
target_link_libraries(custommalloc_static PRIVATE pthread)
target_compile_definitions(custommalloc_static PRIVATE MALLOC_NO_ZONE=1)

# Test targets
function(add_malloc_test name)
    add_executable(${name} tests/${name}.c)
    target_link_libraries(${name} PRIVATE custommalloc_static pthread)
    target_include_directories(${name} PRIVATE include)
endfunction()

add_malloc_test(test_basic)
add_malloc_test(test_realloc)
add_malloc_test(test_large)
add_malloc_test(test_alignment)
add_malloc_test(test_threads)

# Benchmark
add_executable(bench_throughput tests/bench_throughput.c)
target_link_libraries(bench_throughput PRIVATE custommalloc_static pthread)
target_include_directories(bench_throughput PRIVATE include)
