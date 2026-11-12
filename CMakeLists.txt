cmake_minimum_required(VERSION 3.12)
project(tazvera_chain CXX)

set(CMAKE_CXX_STANDARD 17)

include_directories(include)

file(GLOB SOURCES 
    src/crypto.cpp
    src/tx.cpp
    src/state.cpp
    src/mempool.cpp
    src/block.cpp
    src/chain.cpp
    src/main.cpp
)

add_executable(tazvera_node ${SOURCES})
