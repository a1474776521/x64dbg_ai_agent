# PluginPackaging.cmake
# 提供 add_x64dbg_plugin() 以输出 .dp32 / .dp64。
#
# 实质：x64dbg 插件就是普通 dll，只是扩展名为 .dp32 / .dp64。

include(GNUInstallDirs)

function(add_x64dbg_plugin TARGET)
    cmake_parse_arguments(PLG "" "" "SOURCES;LIBS;PRIVATE_INCLUDES;DEFINES" ${ARGN})

    if(NOT PLG_SOURCES)
        message(FATAL_ERROR "add_x64dbg_plugin(${TARGET}) requires SOURCES")
    endif()

    add_library(${TARGET} SHARED ${PLG_SOURCES})

    set_target_properties(${TARGET} PROPERTIES
        PREFIX ""
        SUFFIX ".${X64DbgSDK_PLUGIN_EXT}"
        OUTPUT_NAME "${TARGET}"
    )

    target_link_libraries(${TARGET} PRIVATE
        X64DbgSDK::SDK
        ${PLG_LIBS}
    )

    if(PLG_PRIVATE_INCLUDES)
        target_include_directories(${TARGET} PRIVATE ${PLG_PRIVATE_INCLUDES})
    endif()

    if(PLG_DEFINES)
        target_compile_definitions(${TARGET} PRIVATE ${PLG_DEFINES})
    endif()

    target_compile_definitions(${TARGET} PRIVATE
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN NOMINMAX
        # x64dbg pluginsdk 头里有时引用 _WIN64
        $<$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>:_WIN64>
    )

    # MSVC: 保留 PDB、UTF-8 源文件、关闭部分喧嚣警告
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE
            /utf-8
            /Zi
            /W4
            /wd4100   # unreferenced formal parameter
            /wd4200   # zero-sized array
            /permissive-
        )
        target_link_options(${TARGET} PRIVATE /DEBUG)
    endif()
endfunction()
