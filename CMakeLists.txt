cmake_minimum_required(VERSION 3.5.1)
project(openfhe_benchmarks CXX)
set(CMAKE_CXX_STANDARD 17)

# ── Locate OpenFHE ───────────────────────────────────────────────────────────
find_package(OpenFHE CONFIG REQUIRED)
message(STATUS "OpenFHE Version: ${BASE_OPENFHE_VERSION}")

# ── Compiler flags ───────────────────────────────────────────────────────────
set(CMAKE_CXX_FLAGS "${OpenFHE_CXX_FLAGS} -O3")
string(REPLACE "-Werror" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-result")

# ── Include paths ────────────────────────────────────────────────────────────
include_directories(
    ${OpenFHE_INCLUDE}
    ${OpenFHE_INCLUDE}/third-party/include
    ${OpenFHE_INCLUDE}/core
    ${OpenFHE_INCLUDE}/pke
    ${OpenFHE_INCLUDE}/binfhe
    /opt/profiling-tools/include      # System-installed dram_counter.hpp
)

# ── Benchmark source selection (exactly one) ────────────────────────────────
if(NOT DEFINED BENCH_SOURCE)
    message(FATAL_ERROR
        "Set BENCH_SOURCE to the .cpp file you want to build "
        "(e.g. -DBENCH_SOURCE=examples/rotation.cpp)")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${BENCH_SOURCE}")
    message(FATAL_ERROR "Source file '${BENCH_SOURCE}' not found")
endif()

get_filename_component(TARGET_NAME "${BENCH_SOURCE}" NAME_WE)

add_executable(${TARGET_NAME} "${BENCH_SOURCE}")
target_link_directories(${TARGET_NAME} PRIVATE ${OpenFHE_LIBDIR})
target_link_libraries(${TARGET_NAME} PRIVATE ${OpenFHE_SHARED_LIBRARIES})