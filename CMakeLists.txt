cmake_minimum_required(VERSION 3.20)
project(tazvera_chain_core VERSION 0.8.1 LANGUAGES CXX)

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
