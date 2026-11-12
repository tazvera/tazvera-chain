cmake_minimum_required(VERSION 3.16)
project(tazvera_chain_proto LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)

add_library(tazcore
  src/crypto.cpp
  src/address.cpp
  src/tx.cpp
  src/state.cpp
  src/block.cpp
  src/chain.cpp
  src/json_io.cpp
  src/mempool.cpp
)
target_include_directories(tazcore PUBLIC include)
target_link_libraries(tazcore PUBLIC OpenSSL::Crypto)

add_executable(taz_demo src/main.cpp)
target_link_libraries(taz_demo PRIVATE tazcore)

add_executable(taz_tests tests/test_core.cpp)
target_link_libraries(taz_tests PRIVATE tazcore)

enable_testing()
add_test(NAME core COMMAND taz_tests)
