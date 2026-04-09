cmake_minimum_required(VERSION 3.20)
project(parallel-block-stm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Compiler warnings (always on) ---
add_compile_options(-Wall -Wextra -Wpedantic)

# --- Sanitizer support ---
# Usage: cmake -DSANITIZER=thread|address|undefined
set(SANITIZER "" CACHE STRING "Enable sanitizer (thread, address, undefined)")
if(SANITIZER)
    add_compile_options(-fsanitize=${SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${SANITIZER})
    message(STATUS "Sanitizer enabled: ${SANITIZER}")
endif()

# --- Threading ---
find_package(Threads REQUIRED)

# --- Core engine (header-only interface library) ---
add_library(blockstm_core INTERFACE)
target_include_directories(blockstm_core INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(blockstm_core INTERFACE Threads::Threads)

# --- Third-party (doctest) ---
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

# --- Enable Testing ---
enable_testing()

# --- Tests ---
add_subdirectory(test)

# --- Benchmarks ---
add_subdirectory(bench)
