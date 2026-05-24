cmake_minimum_required(VERSION 3.21)

# vcpkg toolchain 必须在 project() 之前设置（如未通过 -D 传入则尝试自动检测）
if(NOT CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain")
endif()

project(x64dbg-ai-plugin
    VERSION 0.1.0
    DESCRIPTION "AI assistant plugin for x64dbg"
    LANGUAGES C CXX
)

# ============== 全局编译选项 ==============
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# 仅 Windows
if(NOT WIN32)
    message(FATAL_ERROR "x64dbg-ai-plugin 仅支持 Windows")
endif()

# 强制 Release 默认
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ============== 探测依赖 ==============
include(Qt5Setup)              # 设置 Qt5_DIR + find_package(Qt5)
find_package(X64DbgSDK REQUIRED)

find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

include(PluginPackaging)

# ============== sqlite-vec (vector search) ==============
# vcpkg 没有 sqlite-vec port，使用 v0.1.9 amalgamation。
add_library(sqlite_vec STATIC
    "${CMAKE_SOURCE_DIR}/third_party/sqlite-vec/sqlite-vec.c"
)
set_target_properties(sqlite_vec PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
target_include_directories(sqlite_vec PUBLIC
    "${CMAKE_SOURCE_DIR}/third_party/sqlite-vec"
)
target_link_libraries(sqlite_vec PUBLIC unofficial::sqlite3::sqlite3)
target_compile_definitions(sqlite_vec PRIVATE
    SQLITE_CORE
    SQLITE_VEC_STATIC
)
if(MSVC)
    target_compile_options(sqlite_vec PRIVATE /W0)
    # 32 位 MSVC 没有 __popcnt64 intrinsic，强制注入软件实现头。
    target_compile_options(sqlite_vec PRIVATE
        "/FI${CMAKE_SOURCE_DIR}/third_party/sqlite-vec/sqlite-vec-x86-shim.h"
    )
endif()

# ============== 子目录 ==============
add_subdirectory(src)

# 测试（可选）
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# 工具（可选，M-1 等独立 probe 用）
option(BUILD_TOOLS "Build standalone probe/utility tools (tools/*)" OFF)
if(BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# ============== 摘要 ==============
message(STATUS "")
message(STATUS "============================================================")
message(STATUS " x64dbg-ai-plugin ${PROJECT_VERSION}")
message(STATUS "   Arch        : ${X64DbgSDK_ARCH} (.${X64DbgSDK_PLUGIN_EXT})")
message(STATUS "   Qt5         : ${Qt5Core_VERSION} @ ${Qt5_DIR}")
message(STATUS "   Generator   : ${CMAKE_GENERATOR}")
message(STATUS "   Triplet     : ${VCPKG_TARGET_TRIPLET}")
message(STATUS "   Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "============================================================")
message(STATUS "")
