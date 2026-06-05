# =====================================================================
# samples/login_demo/CMakeLists.txt
#
# 独立的 x64dbg 调试样本三件套：plain / xor / antidbg
# 全部 x64-only Qt5.12.12 Widgets 程序，与主插件项目解耦。
#
# 单独构建：
#   cmake -S samples/login_demo -B build-samples-x64 ^
#     -G "Visual Studio 17 2022" -A x64
#   cmake --build build-samples-x64 --config Release
#
# 复用主项目 cmake/Qt5Setup.cmake（探 F:/Qt/5.12.12/msvc2017_64）。
# =====================================================================

cmake_minimum_required(VERSION 3.21)
project(login_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# 复用主项目 Qt5Setup（探 F:/Qt/5.12.12/msvc2017_64）
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
include(Qt5Setup)
# 上面已 find_package(Qt5 ... Widgets ...)

# ---------- 三个独立 exe（共用同一 UI 母版，校验路径各异）----------

# 故意把每个 exe 的源都写成独立 cpp 文件而不抽公共模块，
# 这样逆向时 PDB 里同名函数不会互相干扰，scan_strings 命中也干净。

add_executable(login_demo_plain WIN32 login_demo_plain.cpp)
target_link_libraries(login_demo_plain PRIVATE Qt5::Widgets)
target_compile_definitions(login_demo_plain PRIVATE UNICODE _UNICODE)

add_executable(login_demo_xor WIN32 login_demo_xor.cpp)
target_link_libraries(login_demo_xor PRIVATE Qt5::Widgets)
target_compile_definitions(login_demo_xor PRIVATE UNICODE _UNICODE)

add_executable(login_demo_antidbg WIN32 login_demo_antidbg.cpp)
target_link_libraries(login_demo_antidbg PRIVATE Qt5::Widgets)
target_compile_definitions(login_demo_antidbg PRIVATE UNICODE _UNICODE)

# ---------- 调试样本：禁优化，留符号 ----------
# Release 也加 /Zi 让 x64dbg 能加载 PDB；不剥 symbols
foreach(tgt login_demo_plain login_demo_xor login_demo_antidbg)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /Zi /Od /utf-8)
        target_link_options(${tgt} PRIVATE /DEBUG /OPT:NOREF /OPT:NOICF)
    endif()
endforeach()

# windeployqt：构建后自动把 Qt 运行时 dll + platforms 插件拷过去
find_program(WINDEPLOYQT_EXE windeployqt HINTS "${QT5_ROOT}/msvc2017_64/bin")
if(WINDEPLOYQT_EXE)
    foreach(tgt login_demo_plain login_demo_xor login_demo_antidbg)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${WINDEPLOYQT_EXE}
                --no-translations --no-system-d3d-compiler --no-opengl-sw
                --no-compiler-runtime
                "$<TARGET_FILE:${tgt}>"
            COMMENT "windeployqt for ${tgt}"
            VERBATIM)
    endforeach()
else()
    message(WARNING "windeployqt not found at ${QT5_ROOT}/msvc2017_64/bin")
endif()

message(STATUS "login_demo: Qt5=${Qt5_DIR} / plain+xor+antidbg (x64 only, /Od + /Zi)")
