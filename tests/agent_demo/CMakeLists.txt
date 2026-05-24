# tests/agent_demo/CMakeLists.txt
#
# Agent 工具调用 / 预设右键 / ReasoningBlock 验证目标进程。
# 故意 /Od 保留所有调用与字节序列，便于工具调用命中。
cmake_minimum_required(VERSION 3.20)
project(agent_demo CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(agent_demo
    main.cpp
    scenario_decode.cpp
    scenario_crc.cpp
    scenario_state_machine.cpp
    scenario_magic.cpp
)

if(MSVC)
    # /Od 不优化，/Zi 生成 pdb，/utf-8 让中文注释正确解析
    target_compile_options(agent_demo PRIVATE /Od /Zi /EHsc /utf-8)
    target_link_options(agent_demo PRIVATE /DEBUG /INCREMENTAL:NO)
else()
    target_compile_options(agent_demo PRIVATE -O0 -g)
endif()
