cmake_minimum_required(VERSION 3.10)
project(OpenFHEBenchmarks CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenFHE
find_package(OpenFHE CONFIG REQUIRED)
message(STATUS "OpenFHE Version: ${BASE_OPENFHE_VERSION}")

# Compiler flags
set(CMAKE_CXX_FLAGS "${OpenFHE_CXX_FLAGS} -O3 -march=native -mtune=native -fopenmp")
string(REPLACE "-Werror" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-result")

# Include paths
include_directories(
    ${OpenFHE_INCLUDE}
    ${OpenFHE_INCLUDE}/third-party/include
    ${OpenFHE_INCLUDE}/core
    ${OpenFHE_INCLUDE}/pke
    ${OpenFHE_INCLUDE}/binfhe
    ${CMAKE_CURRENT_SOURCE_DIR}/examples
    /opt/profiling-tools/include
)

# Benchmark source selection
if(NOT DEFINED BENCH_SOURCE)
    message(FATAL_ERROR "Set BENCH_SOURCE to the .cpp file you want to build (e.g. -DBENCH_SOURCE=examples/addition.cpp)")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${BENCH_SOURCE}")
    message(FATAL_ERROR "Source file '${BENCH_SOURCE}' not found")
endif()

# Extract benchmark name and create executable
get_filename_component(BENCH_NAME "${BENCH_SOURCE}" NAME_WE)
add_executable(${BENCH_NAME} "${BENCH_SOURCE}")

# Link libraries
target_link_directories(${BENCH_NAME} PRIVATE ${OpenFHE_LIBDIR})
target_link_libraries(${BENCH_NAME} PRIVATE ${OpenFHE_SHARED_LIBRARIES})