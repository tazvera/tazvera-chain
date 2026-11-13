cmake_minimum_required(VERSION 3.20)
project(tazvera_chain_core VERSION 0.4.0 LANGUAGES CXX)

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
