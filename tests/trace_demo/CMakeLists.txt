# tests/trace_demo/CMakeLists.txt
#
# 独立的小 demo，便于直接 cmake -B build-demo -S . 编译。
# 故意 -Od / /Od（不优化），保留所有 call 指令以利 trace。
cmake_minimum_required(VERSION 3.20)
project(trace_demo CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(trace_demo
    main.cpp
    business.cpp
    math_ops.cpp
)

if(MSVC)
    # Release 也禁优化 + 带调试符号；/utf-8 让 cl 正确解析 UTF-8 源文件中文注释
    target_compile_options(trace_demo PRIVATE /Od /Zi /EHsc /utf-8)
    target_link_options(trace_demo PRIVATE /DEBUG /INCREMENTAL:NO)
else()
    target_compile_options(trace_demo PRIVATE -O0 -g)
endif()
