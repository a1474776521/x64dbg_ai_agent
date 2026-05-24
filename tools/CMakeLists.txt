# tools/CMakeLists.txt
# 独立小工具集合。每个工具自成 target，与主插件解耦。
# 默认不构建，需 -DBUILD_TOOLS=ON 启用，避免日常 rebuild 拖速度。

# probe_reasoning：M-1 复核 DeepSeek reasoning_content 回传方向。
add_executable(probe_reasoning
    probe_reasoning.cpp
)
target_link_libraries(probe_reasoning PRIVATE
    cpr::cpr
    nlohmann_json::nlohmann_json
)
target_compile_features(probe_reasoning PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(probe_reasoning PRIVATE /utf-8 /W3 /permissive-)
endif()
set_target_properties(probe_reasoning PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)
