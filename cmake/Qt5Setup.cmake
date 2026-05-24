# Qt5Setup.cmake
# 自动探测 Qt 5.12.12 安装位置（msvc2017 或 msvc2017_64），
# 优先使用 -DQt5_DIR，否则按架构在常见路径下查找。

if(NOT DEFINED QT5_ROOT)
    set(QT5_ROOT "F:/Qt/5.12.12" CACHE PATH "Qt 5.12.12 install root")
endif()

if(NOT Qt5_DIR)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_qt_dir "${QT5_ROOT}/msvc2017_64/lib/cmake/Qt5")
    else()
        set(_qt_dir "${QT5_ROOT}/msvc2017/lib/cmake/Qt5")
    endif()
    if(EXISTS "${_qt_dir}/Qt5Config.cmake")
        set(Qt5_DIR "${_qt_dir}" CACHE PATH "Qt5 cmake dir" FORCE)
    else()
        message(FATAL_ERROR
            "Qt5 cmake config not found: ${_qt_dir}\n"
            "请运行 scripts\\install_qt.ps1 安装 Qt 5.12.12，"
            "或显式指定 -DQt5_DIR=...")
    endif()
endif()

# Qt 5.12 需要这些以正确 link Windows GUI
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 5.12.12 EXACT REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    Concurrent
)

# 反向校验版本
if(NOT Qt5Core_VERSION VERSION_EQUAL "5.12.12")
    message(FATAL_ERROR
        "Qt 版本必须严格等于 5.12.12（与 x64dbg snapshot 对齐），"
        "实际找到 ${Qt5Core_VERSION}")
endif()

message(STATUS "Qt5: ${Qt5_DIR}  version=${Qt5Core_VERSION}")
