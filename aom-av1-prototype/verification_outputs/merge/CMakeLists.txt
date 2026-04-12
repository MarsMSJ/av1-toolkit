cmake_minimum_required(VERSION 3.16)
project(av1_console_decoder C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Find libaom — either from a local build or system install
# Pass -DAOM_BUILD_DIR=<path> to point at your libaom build directory.
# ---------------------------------------------------------------------------

set(AOM_BUILD_DIR "" CACHE PATH "Path to libaom cmake build directory")
set(AOM_SOURCE_DIR "" CACHE PATH "Path to libaom source (for headers)")

# Try to find libaom via pkg-config first
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(AOM QUIET aom)
endif()

if(NOT AOM_FOUND)
    if(AOM_BUILD_DIR AND EXISTS "${AOM_BUILD_DIR}/libaom.a")
        set(AOM_LIBRARIES "${AOM_BUILD_DIR}/libaom.a")
        set(AOM_INCLUDE_DIRS "")
    elseif(AOM_BUILD_DIR AND EXISTS "${AOM_BUILD_DIR}/libaom.so")
        set(AOM_LIBRARIES "${AOM_BUILD_DIR}/libaom.so")
        set(AOM_INCLUDE_DIRS "")
    else()
        message(FATAL_ERROR
            "libaom not found. Either:\n"
            "  1. Install libaom system-wide, or\n"
            "  2. Build libaom and pass -DAOM_BUILD_DIR=<build_dir> -DAOM_SOURCE_DIR=<source_dir>\n"
            "\n"
            "To build libaom:\n"
            "  cd ~/dev-tools/aom && mkdir -p build && cd build\n"
            "  cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_EXAMPLES=0 -DENABLE_TESTS=0\n"
            "  make -j$(nproc)\n"
        )
    endif()
endif()

# AOM source headers (aom/aom_decoder.h, av1/decoder/decoder.h, etc.)
if(AOM_SOURCE_DIR)
    list(APPEND AOM_INCLUDE_DIRS "${AOM_SOURCE_DIR}")
    # The build dir also generates config headers (aom_config.h, aom_version.h)
    if(AOM_BUILD_DIR)
        list(APPEND AOM_INCLUDE_DIRS "${AOM_BUILD_DIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Console decoder library
# ---------------------------------------------------------------------------

add_library(av1_console_decoder STATIC
    av1_decoder_api.c
    av1_mem_override.c
    av1_job_queue.c
    av1_copy_thread.c
    av1_gpu_thread.c
)

target_include_directories(av1_console_decoder
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${AOM_INCLUDE_DIRS}
)

target_link_libraries(av1_console_decoder
    PUBLIC ${AOM_LIBRARIES}
    PUBLIC pthread
    PUBLIC m
)

# Warnings
target_compile_options(av1_console_decoder PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)

# ---------------------------------------------------------------------------
# IVF parser + Y4M writer (utilities)
# ---------------------------------------------------------------------------

add_library(av1_utils STATIC
    ivf_parser.c
    y4m_writer.c
)

target_include_directories(av1_utils
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---------------------------------------------------------------------------
# Test executables
# ---------------------------------------------------------------------------

# End-to-end test: decode IVF → Y4M
add_executable(test_e2e test_e2e.c)
target_link_libraries(test_e2e PRIVATE av1_console_decoder av1_utils)

# Unit tests
add_executable(test_mem_override test_mem_override.c)
target_link_libraries(test_mem_override PRIVATE av1_console_decoder)

add_executable(test_job_queue test_job_queue.c)
target_link_libraries(test_job_queue PRIVATE av1_console_decoder)

add_executable(test_copy_thread test_copy_thread.c)
target_link_libraries(test_copy_thread PRIVATE av1_console_decoder)

add_executable(test_gpu_thread test_gpu_thread.c)
target_link_libraries(test_gpu_thread PRIVATE av1_console_decoder)

# IVF test file generator
add_executable(generate_test_ivf generate_test_ivf.c)
target_link_libraries(generate_test_ivf PRIVATE av1_utils)

# ---------------------------------------------------------------------------
# Convenience target: run all unit tests
# ---------------------------------------------------------------------------

enable_testing()
add_test(NAME mem_override COMMAND test_mem_override)
add_test(NAME job_queue    COMMAND test_job_queue)
add_test(NAME copy_thread  COMMAND test_copy_thread)
add_test(NAME gpu_thread   COMMAND test_gpu_thread)
