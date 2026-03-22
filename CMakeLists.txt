cmake_minimum_required(VERSION 3.16)
project(MarketDataFeedHandler CXX)

# ─────────────────────────────────────────────────────────────────────────────
# Compiler settings
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(COMMON_FLAGS
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -fno-omit-frame-pointer
)

set(RELEASE_FLAGS
    -O3
    -march=native
    -flto
    -DNDEBUG
)

set(DEBUG_FLAGS
    -O0 -g3
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
)

add_compile_options(${COMMON_FLAGS})

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(${RELEASE_FLAGS})
    add_link_options(-flto)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(${DEBUG_FLAGS})
    add_link_options(-fsanitize=address,undefined)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Include paths
# ─────────────────────────────────────────────────────────────────────────────

include_directories(${CMAKE_SOURCE_DIR}/include)

# ─────────────────────────────────────────────────────────────────────────────
# Common library (cache, latency tracker)
# ─────────────────────────────────────────────────────────────────────────────

add_library(common STATIC
    src/common/cache.cpp
    src/common/latency_tracker.cpp
)
target_include_directories(common PUBLIC include)
target_link_libraries(common PUBLIC pthread)

# ─────────────────────────────────────────────────────────────────────────────
# Server library + executable
# ─────────────────────────────────────────────────────────────────────────────

add_library(server_lib STATIC
    src/server/tick_generator.cpp
    src/server/client_manager.cpp
    src/server/exchange_simulator.cpp
)
target_include_directories(server_lib PUBLIC include)
target_link_libraries(server_lib PUBLIC common)

add_executable(exchange_server src/server/main.cpp)
target_link_libraries(exchange_server PRIVATE server_lib common pthread)

# ─────────────────────────────────────────────────────────────────────────────
# Client library + executable
# ─────────────────────────────────────────────────────────────────────────────

add_library(client_lib STATIC
    src/client/socket.cpp
    src/client/parser.cpp
    src/client/visualizer.cpp
    src/client/feed_handler.cpp
)
target_include_directories(client_lib PUBLIC include)
target_link_libraries(client_lib PUBLIC common)

add_executable(feed_client src/client/main.cpp)
target_link_libraries(feed_client PRIVATE client_lib common pthread)

# ─────────────────────────────────────────────────────────────────────────────
# Tests (Google Test)
# ─────────────────────────────────────────────────────────────────────────────

option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    find_package(GTest QUIET)

    if(GTest_FOUND)
        enable_testing()

        add_executable(run_tests
            tests/test_cache.cpp
            tests/test_parser.cpp
            tests/test_latency_tracker.cpp
            tests/test_ring_buffer.cpp
            tests/test_protocol.cpp
        )
        target_include_directories(run_tests PRIVATE include)
        target_link_libraries(run_tests PRIVATE
            common client_lib server_lib
            GTest::GTest GTest::Main
            pthread
        )
        add_test(NAME AllTests COMMAND run_tests)
    else()
        message(STATUS "GTest not found — skipping tests. Install with: sudo apt install libgtest-dev")
    endif()
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Benchmarks
# ─────────────────────────────────────────────────────────────────────────────

option(BUILD_BENCHMARKS "Build microbenchmarks" ON)

if(BUILD_BENCHMARKS)
    add_executable(bench_parser   benchmarks/bench_parser.cpp)
    target_link_libraries(bench_parser   PRIVATE client_lib common pthread)

    add_executable(bench_cache    benchmarks/bench_cache.cpp)
    target_link_libraries(bench_cache    PRIVATE common pthread)

    add_executable(bench_latency  benchmarks/bench_latency.cpp)
    target_link_libraries(bench_latency  PRIVATE common pthread)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Install targets
# ─────────────────────────────────────────────────────────────────────────────

install(TARGETS exchange_server feed_client
        RUNTIME DESTINATION bin)

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────

message(STATUS "Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "CXX:         ${CMAKE_CXX_COMPILER}")
message(STATUS "Tests:       ${BUILD_TESTS}")
message(STATUS "Benchmarks:  ${BUILD_BENCHMARKS}")
