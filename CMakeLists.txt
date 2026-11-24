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
    src/startup.cpp
    src/hello.cpp
    src/net.cpp
    src/fork.cpp
    src/auth.cpp
)
target_include_directories(tazcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tazcore PUBLIC OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(tazcore PUBLIC ws2_32)   # v9.0: sockets on Windows
endif()
target_compile_options(tazcore PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

# v9.0: the first node binary. It loads a chain through the pinned startup path and prints what it loaded;
# it does not produce blocks or talk to anyone yet.
add_executable(taznode tools/taznode/main.cpp)
target_link_libraries(taznode PRIVATE tazcore)
add_test(NAME taznode_help COMMAND taznode --help)

# v9.0: drive the real binary as a process — create a chain, load it with the right pin, refuse a wrong,
# malformed or missing pin. The library test proves the function; this proves the program.
add_executable(taz_node_binary_tests tests/test_node_binary.cpp)
target_link_libraries(taz_node_binary_tests PRIVATE tazcore)
target_compile_definitions(taz_node_binary_tests PRIVATE TAZ_NODE_BINARY="$<TARGET_FILE:taznode>")
add_dependencies(taz_node_binary_tests taznode)
add_test(NAME taz_node_binary_tests COMMAND taz_node_binary_tests)
add_test(NAME taznode_refuses_without_pin COMMAND taznode --chain /nonexistent.json)
set_tests_properties(taznode_refuses_without_pin PROPERTIES WILL_FAIL TRUE)

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

add_executable(taz_fork_tests tests/test_fork.cpp)
target_link_libraries(taz_fork_tests PRIVATE tazcore)
add_test(NAME taz_fork_tests COMMAND taz_fork_tests)

add_executable(taz_net_tests tests/test_net.cpp)
target_link_libraries(taz_net_tests PRIVATE tazcore)
add_test(NAME taz_net_tests COMMAND taz_net_tests)

add_executable(taz_hello_tests tests/test_hello.cpp)
target_link_libraries(taz_hello_tests PRIVATE tazcore)
add_test(NAME taz_hello_tests COMMAND taz_hello_tests)

add_executable(taz_startup_tests tests/test_startup.cpp)
target_link_libraries(taz_startup_tests PRIVATE tazcore)
add_test(NAME taz_startup_tests COMMAND taz_startup_tests)

add_executable(taz_producer_tests tests/test_producer.cpp)
target_link_libraries(taz_producer_tests PRIVATE tazcore)
add_test(NAME taz_producer_tests COMMAND taz_producer_tests)

# v9.1: regression tests for the network problems found in v9.0 (a legal block too big for one message, a block over the
# sync budget that never syncs, a future-dated block that jams a chain, a silently ignored --transfer).
find_package(Threads REQUIRED)
add_executable(taz_auth_tests tests/test_auth.cpp)
target_link_libraries(taz_auth_tests PRIVATE tazcore)
add_test(NAME taz_auth_tests COMMAND taz_auth_tests)

add_executable(taz_bigblock_tests tests/test_bigblock.cpp)
target_link_libraries(taz_bigblock_tests PRIVATE tazcore)
add_test(NAME taz_bigblock_tests COMMAND taz_bigblock_tests)

add_executable(taz_hardening91_tests tests/test_hardening91.cpp)
target_link_libraries(taz_hardening91_tests PRIVATE tazcore Threads::Threads)
target_compile_definitions(taz_hardening91_tests PRIVATE TAZ_NODE_BINARY="$<TARGET_FILE:taznode>")
add_dependencies(taz_hardening91_tests taznode)
add_test(NAME taz_hardening91_tests COMMAND taz_hardening91_tests)

add_executable(taz_security82_tests tests/test_security82.cpp)
target_link_libraries(taz_security82_tests PRIVATE tazcore)
add_test(NAME taz_security82_tests COMMAND taz_security82_tests)
# The deterministic JSON/transaction fuzz suite is intentionally heavier under ASan/UBSan.
# Give this one security test a bounded 5-minute budget; other tests keep the 120-second default.
set_tests_properties(taz_security82_tests PROPERTIES TIMEOUT 300)

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

# F2: a chain the node saved must always load again (exact case count so a silent skip cannot pass)
add_executable(taz_persist_limit_tests tests/test_persist_limits.cpp)
target_link_libraries(taz_persist_limit_tests PRIVATE tazcore)
add_test(NAME taz_persist_limit_tests COMMAND taz_persist_limit_tests)
set_tests_properties(taz_persist_limit_tests PROPERTIES PASS_REGULAR_EXPRESSION "persist limit tests: all passed \\(6 cases\\)" TIMEOUT 600)
