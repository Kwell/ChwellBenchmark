cmake_minimum_required(VERSION 3.11)

project(ChwellBenchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CHWELL_BENCH_ENABLE_LTO "Enable LTO for benchmarks" OFF)

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3)
else()
    add_compile_options(-O0 -g)
endif()

if (CHWELL_BENCH_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported OUTPUT lto_output)
    if (lto_supported)
        message(STATUS "IPO/LTO enabled for benchmarks")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "IPO/LTO not supported: ${lto_output}")
    endif()
endif()

add_executable(echo_qps_bench
    src/echo_qps_bench.cpp
)

target_compile_definitions(echo_qps_bench PRIVATE _GNU_SOURCE)

