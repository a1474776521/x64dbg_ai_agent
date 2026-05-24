# tests/reasoning_demo/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(reasoning_demo CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(reasoning_demo main.cpp)

if(MSVC)
    target_compile_options(reasoning_demo PRIVATE /Od /Zi /EHsc /utf-8)
    target_link_options(reasoning_demo PRIVATE /DEBUG /INCREMENTAL:NO)
else()
    target_compile_options(reasoning_demo PRIVATE -O0 -g)
endif()
