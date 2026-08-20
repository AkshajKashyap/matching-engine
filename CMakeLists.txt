cmake_minimum_required(VERSION 3.20)

project(matching_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MATCHING_ENGINE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MATCHING_ENGINE_ENABLE_TSAN "Enable ThreadSanitizer for gateway concurrency tests" OFF)
option(MATCHING_ENGINE_BUILD_BENCHMARKS "Build baseline benchmark and profiling executables" ON)

if(MATCHING_ENGINE_ENABLE_SANITIZERS AND MATCHING_ENGINE_ENABLE_TSAN)
    message(FATAL_ERROR "ASan/UBSan and ThreadSanitizer cannot be enabled in the same build")
endif()

add_library(matching_engine
    src/durable_engine.cpp
    src/journal.cpp
    src/journal_storage.cpp
    src/order_book.cpp
)

target_include_directories(matching_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

function(enable_project_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
    endif()
endfunction()

function(enable_project_sanitizers target)
    if(MATCHING_ENGINE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined
        )
    endif()
endfunction()

function(enable_project_thread_sanitizer target)
    if(MATCHING_ENGINE_ENABLE_TSAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -fsanitize=thread
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=thread
        )
    endif()
endfunction()

enable_project_warnings(matching_engine)
enable_project_sanitizers(matching_engine)

# The gateway layer currently contains only a pure binary wire codec. Keeping
# it separate prevents future POSIX networking concerns from leaking into the
# deterministic matching and durability core.
add_library(matching_gateway
    src/exchange_server.cpp
    src/gateway_server.cpp
    src/gateway_types.cpp
    src/wire_protocol.cpp
)
target_link_libraries(matching_gateway PUBLIC matching_engine)
target_include_directories(matching_gateway
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
enable_project_warnings(matching_gateway)
enable_project_sanitizers(matching_gateway)
enable_project_thread_sanitizer(matching_gateway)

if(MATCHING_ENGINE_BUILD_BENCHMARKS)
    include(FetchContent)
    find_package(Threads REQUIRED)

    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG 344117638c8ff7e239044fd0fa7085839fc03021 # v1.8.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googlebenchmark)

    add_executable(order_book_benchmark
        benchmarks/order_book_benchmark.cpp
    )
    target_link_libraries(order_book_benchmark PRIVATE matching_engine benchmark::benchmark)
    enable_project_warnings(order_book_benchmark)
    enable_project_sanitizers(order_book_benchmark)

    add_executable(order_book_profile_workload
        benchmarks/profile_workload.cpp
    )
    target_link_libraries(order_book_profile_workload PRIVATE matching_engine)
    enable_project_warnings(order_book_profile_workload)
    enable_project_sanitizers(order_book_profile_workload)

    add_executable(durable_engine_benchmark
        benchmarks/durable_engine_benchmark.cpp
    )
    target_link_libraries(durable_engine_benchmark PRIVATE matching_engine benchmark::benchmark)
    enable_project_warnings(durable_engine_benchmark)
    enable_project_sanitizers(durable_engine_benchmark)

    add_executable(network_exchange_benchmark
        benchmarks/network_exchange_benchmark.cpp
    )
    target_link_libraries(network_exchange_benchmark PRIVATE matching_gateway Threads::Threads)
    enable_project_warnings(network_exchange_benchmark)
    enable_project_sanitizers(network_exchange_benchmark)
    enable_project_thread_sanitizer(network_exchange_benchmark)
endif()

include(CTest)

if(BUILD_TESTING)
    include(FetchContent)

    find_package(Threads REQUIRED)

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571 # v1.14.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(matching_engine_tests
        tests/durable_engine_test.cpp
        tests/durable_engine_stress_test.cpp
        tests/journal_test.cpp
        tests/journal_storage_test.cpp
        tests/order_book_test.cpp
        tests/order_book_differential_test.cpp
        tests/bounded_queue_test.cpp
        tests/gateway_types_test.cpp
        tests/gateway_server_test.cpp
        tests/exchange_server_test.cpp
        tests/wire_protocol_test.cpp
    )

    target_link_libraries(matching_engine_tests
        PRIVATE
            matching_engine
            matching_gateway
            GTest::gtest_main
            Threads::Threads
    )
    target_include_directories(matching_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    enable_project_warnings(matching_engine_tests)
    enable_project_sanitizers(matching_engine_tests)
    enable_project_thread_sanitizer(matching_engine_tests)

    include(GoogleTest)
    gtest_add_tests(TARGET matching_engine_tests TEST_LIST matching_engine_test_cases)
    if(MATCHING_ENGINE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # This execution environment runs tests under ptrace, which is not
        # compatible with LeakSanitizer. ASan and UBSan remain enabled.
        set_tests_properties(${matching_engine_test_cases} PROPERTIES
            ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
        )
    endif()
endif()
