cmake_minimum_required(VERSION 3.25)

project(pin
    VERSION 0.1.0
    DESCRIPTION "Header-only C++23 module coordination and dependency injection library"
    LANGUAGES CXX)

add_library(pin INTERFACE)
add_library(pin::pin ALIAS pin)

target_include_directories(pin INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(pin INTERFACE cxx_std_23)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pin INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
endif()

if(PIN_SANITIZE)
    add_compile_options(-fsanitize=${PIN_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${PIN_SANITIZE})
endif()

option(PIN_ENABLE_TIDY "Run clang-tidy on every compilation" OFF)
if(PIN_ENABLE_TIDY)
    find_program(PIN_CLANG_TIDY NAMES clang-tidy)
    if(PIN_CLANG_TIDY)
        set(CMAKE_CXX_CLANG_TIDY "${PIN_CLANG_TIDY}")
    else()
        message(WARNING "PIN_ENABLE_TIDY=ON but clang-tidy was not found")
    endif()
endif()

option(PIN_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(PIN_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
option(PIN_BUILD_BENCHMARKS "Build the benchmarks (fetches Google Benchmark)" OFF)

if(PIN_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.12)
    FetchContent_MakeAvailable(doctest)

    add_executable(pin_tests
        tests/test_main.cpp
        tests/test_di.cpp
        tests/test_scope.cpp
        tests/test_lifecycle.cpp
        tests/test_threads.cpp)
    target_link_libraries(pin_tests PRIVATE pin doctest::doctest)
endif()

if(PIN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(PIN_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
