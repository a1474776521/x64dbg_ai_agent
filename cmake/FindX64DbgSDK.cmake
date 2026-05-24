# FindX64DbgSDK.cmake
# 提供 x64dbg pluginsdk 的导入目标。
#
# 使用：
#   find_package(X64DbgSDK REQUIRED)
#   target_link_libraries(my_plugin PRIVATE X64DbgSDK::SDK)
#
# 变量：
#   X64DbgSDK_ROOT          覆盖 SDK 根目录（默认 ${PROJECT_SOURCE_DIR}/third_party/pluginsdk）
#
# 根据当前架构（x86 / x64）自动选择 x32dbg.lib + x32bridge.lib
# 或 x64dbg.lib + x64bridge.lib。

if(NOT DEFINED X64DbgSDK_ROOT)
    set(X64DbgSDK_ROOT "${PROJECT_SOURCE_DIR}/third_party/pluginsdk"
        CACHE PATH "x64dbg plugin SDK root")
endif()

if(NOT EXISTS "${X64DbgSDK_ROOT}/_plugins.h")
    message(FATAL_ERROR
        "x64dbg pluginsdk not found at ${X64DbgSDK_ROOT}\n"
        "请将 x64dbg snapshot 中的 pluginsdk 目录拷贝到 third_party/pluginsdk")
endif()

# 根据架构选 lib
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_x64dbg_lib    "${X64DbgSDK_ROOT}/x64dbg.lib")
    set(_x64dbg_bridge "${X64DbgSDK_ROOT}/x64bridge.lib")
    set(X64DbgSDK_ARCH "x64" CACHE STRING "" FORCE)
    set(X64DbgSDK_PLUGIN_EXT "dp64" CACHE STRING "" FORCE)
else()
    set(_x64dbg_lib    "${X64DbgSDK_ROOT}/x32dbg.lib")
    set(_x64dbg_bridge "${X64DbgSDK_ROOT}/x32bridge.lib")
    set(X64DbgSDK_ARCH "x32" CACHE STRING "" FORCE)
    set(X64DbgSDK_PLUGIN_EXT "dp32" CACHE STRING "" FORCE)
endif()

foreach(_lib IN LISTS _x64dbg_lib _x64dbg_bridge)
    if(NOT EXISTS "${_lib}")
        message(FATAL_ERROR "x64dbg SDK lib 缺失: ${_lib}")
    endif()
endforeach()

add_library(X64DbgSDK::SDK INTERFACE IMPORTED)
target_include_directories(X64DbgSDK::SDK INTERFACE "${X64DbgSDK_ROOT}")
target_link_libraries(X64DbgSDK::SDK INTERFACE
    "${_x64dbg_lib}"
    "${_x64dbg_bridge}"
)
# pluginsdk 的头里直接调 Bridge 函数，需要 BUILD_SHARED_LIBS 风格的导出宏
target_compile_definitions(X64DbgSDK::SDK INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

set(X64DbgSDK_FOUND TRUE)

message(STATUS "X64DbgSDK: ${X64DbgSDK_ROOT}  arch=${X64DbgSDK_ARCH}  ext=.${X64DbgSDK_PLUGIN_EXT}")
