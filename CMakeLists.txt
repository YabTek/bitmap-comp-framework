cmake_minimum_required(VERSION 3.20)
project(bitmap_comparison VERSION 0.2.0 LANGUAGES CXX)

# No external dependencies: a C++17 compiler and CMake are all you need.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(CMAKE_SIZEOF_VOID_P LESS 8)
  message(FATAL_ERROR "This framework requires a 64-bit environment")
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Always measure in Release. A Debug build is one to two orders of magnitude
# slower and the backends do not degrade uniformly, so Debug numbers say
# nothing about the representations.
option(BENCH_NATIVE "Compile for this exact CPU (-march=native)" OFF)
option(BENCH_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
if(BENCH_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
  if(BENCH_NATIVE)
    add_compile_options(-march=native)
  endif()
endif()

add_library(bench_common STATIC
  common/bitmap_io.cpp
  common/query.cpp
)
target_include_directories(bench_common PUBLIC ${CMAKE_SOURCE_DIR})

add_library(bench_backends STATIC
  backends/raw_backend.cpp
  backends/roaring_backend.cpp
  backends/roaring_bitmap.cpp
  backends/wah_backend.cpp
  backends/archive.cpp
  backends/codecs.cpp
  backends/word_codecs.cpp
  backends/byte_codecs.cpp
  backends/stream_backend.cpp
  backends/factory.cpp
)
target_include_directories(bench_backends PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(bench_backends PUBLIC bench_common)

add_executable(generate_queries tools/generate_queries.cpp)
target_link_libraries(generate_queries PRIVATE bench_common)

add_executable(bitmap_bench benchmarks/bitmap_bench.cpp)
target_link_libraries(bitmap_bench PRIVATE bench_backends)
target_compile_definitions(bitmap_bench PRIVATE
  BENCH_VERSION="${PROJECT_VERSION}"
  BENCH_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
  BENCH_BUILD_TYPE="$<CONFIG>"
)

add_executable(bitmap_inspect tools/bitmap_inspect.cpp)
target_link_libraries(bitmap_inspect PRIVATE bench_backends)

include(CTest)
if(BUILD_TESTING)
  add_executable(codec_tests tests/codec_tests.cpp)
  target_link_libraries(codec_tests PRIVATE bench_backends)
  add_test(NAME codec_correctness COMMAND codec_tests)
  add_executable(io_tests tests/io_tests.cpp)
  target_link_libraries(io_tests PRIVATE bench_common)
  add_test(NAME input_output COMMAND io_tests ${CMAKE_BINARY_DIR}/io-test-output)
  add_test(NAME cli_smoke COMMAND ${CMAKE_COMMAND}
    -DBENCH=$<TARGET_FILE:bitmap_bench>
    -DGENERATOR=$<TARGET_FILE:generate_queries>
    -DINSPECTOR=$<TARGET_FILE:bitmap_inspect>
    -DDATA=${CMAKE_SOURCE_DIR}/data/example
    -DOUTPUT=${CMAKE_BINARY_DIR}/test-output
    -P ${CMAKE_SOURCE_DIR}/tests/cli_smoke.cmake)
endif()

# Task 1: your dataset generator goes here, e.g.
#
#   add_executable(generate_dataset tools/generate_dataset.cpp)
#   target_link_libraries(generate_dataset PRIVATE bench_common)
#
# bench_common gives you write_metadata() / write_bitmap() from
# common/bitmap_io.hpp, which produce exactly the format the benchmark reads.

add_executable(generate_dataset tools/generate_dataset.cpp)
target_link_libraries(generate_dataset PRIVATE bench_common)
