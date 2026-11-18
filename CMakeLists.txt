cmake_minimum_required(VERSION 3.20)

# v8.9: the version lives in include/taz/types.hpp (CORE_VERSION) and nowhere else. taz_persist_tests
# compares it with the README title, so a drifted document fails CI instead of surviving a review.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/taz/types.hpp" TAZ_TYPES_HPP)
string(REGEX MATCH "CORE_VERSION = \"([0-9]+\\.[0-9]+)\"" _taz_version_match "${TAZ_TYPES_HPP}")
set(TAZ_CORE_VERSION "${CMAKE_MATCH_1}")
if(NOT TAZ_CORE_VERSION)
    message(FATAL_ERROR "CORE_VERSION not found in include/taz/types.hpp")
endif()
message(STATUS "TAZVERA core version ${TAZ_CORE_VERSION}")

project(tazvera_chain_core VERSION ${TAZ_CORE_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

add_library(tazcore
    src/crypto.cpp
    src/tx.cpp
    src/state.cpp
    src/block.cpp
    src/chain.cpp
    src/export.cpp
    src/mempool.cpp
    src/persist.cpp
)
target_include_directories(tazcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tazcore PUBLIC OpenSSL::Crypto)
target_compile_options(tazcore PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(taz_demo src/main.cpp)
target_link_libraries(taz_demo PRIVATE tazcore)

enable_testing()
add_executable(taz_tests tests/test_core.cpp)
target_link_libraries(taz_tests PRIVATE tazcore)
add_test(NAME taz_core_tests COMMAND taz_tests)

add_executable(taz_export_demo src/export_demo.cpp)
target_link_libraries(taz_export_demo PRIVATE tazcore)

add_executable(taz_export_tests tests/test_export.cpp)
target_link_libraries(taz_export_tests PRIVATE tazcore)
add_test(NAME taz_export_tests COMMAND taz_export_tests)

add_executable(taz_safety_tests tests/test_safety.cpp)
target_link_libraries(taz_safety_tests PRIVATE tazcore)
add_test(NAME taz_safety_tests COMMAND taz_safety_tests)

add_executable(taz_recovery_tests tests/test_recovery.cpp)
target_link_libraries(taz_recovery_tests PRIVATE tazcore)
add_test(NAME taz_recovery_tests COMMAND taz_recovery_tests)

add_executable(taz_mempool_tests tests/test_mempool.cpp)
target_link_libraries(taz_mempool_tests PRIVATE tazcore)
add_test(NAME taz_mempool_tests COMMAND taz_mempool_tests)

add_executable(taz_persist_tests tests/test_persist.cpp)
find_package(Threads REQUIRED)
target_link_libraries(taz_persist_tests PRIVATE tazcore Threads::Threads)
target_compile_definitions(taz_persist_tests PRIVATE TAZ_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME taz_persist_tests COMMAND taz_persist_tests)

add_executable(taz_security82_tests tests/test_security82.cpp)
target_link_libraries(taz_security82_tests PRIVATE tazcore)
add_test(NAME taz_security82_tests COMMAND taz_security82_tests)

# v8.3: libFuzzer targets. Off by default; the "Fuzz (libFuzzer)" workflow turns them on with clang.
option(TAZ_BUILD_FUZZERS "Build the libFuzzer targets (clang, -fsanitize=fuzzer)" OFF)
if(TAZ_BUILD_FUZZERS)
    foreach(target fuzz_load_chain fuzz_tx)
        add_executable(${target} tests/fuzz/${target}.cpp)
        target_link_libraries(${target} PRIVATE tazcore)
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
endif()

# v8.10: writes chain files that exercise the veto rules, so the explorer cross-check can be run against
# chains the C++ core itself produced (not only against the three shipped demos).
add_executable(taz_gen_veto_demo tests/gen_veto_demo.cpp)
target_link_libraries(taz_gen_veto_demo PRIVATE tazcore)
