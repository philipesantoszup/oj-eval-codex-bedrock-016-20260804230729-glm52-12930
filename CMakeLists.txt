cmake_minimum_required(VERSION 3.10)
project(FileStorageBPT CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use g++-13
set(CMAKE_CXX_COMPILER g++-13)

# Optimization flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -std=c++17")

add_executable(code code.cpp)
