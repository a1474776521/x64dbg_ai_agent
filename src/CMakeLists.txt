set(PLUGIN_NAME "x64dbg_ai_plugin")

# 资源文件（M1 阶段最小）
set(QRC_FILES
    "${CMAKE_SOURCE_DIR}/resources/resources.qrc"
)

set(PLUGIN_SOURCES
    # plugin layer
    plugin/plugin_main.cpp
    plugin/plugin_callbacks.cpp
    plugin/plugin_callbacks.h
    plugin/plugin_menus.cpp
    plugin/plugin_menus.h

    # ai
    ai/chat_provider.h
    ai/copilot_auth.cpp
    ai/copilot_auth.h
    ai/copilot_chat_client.cpp
    ai/copilot_chat_client.h
    ai/deepseek_chat_client.cpp
    ai/deepseek_chat_client.h
    ai/provider_manager.cpp
    ai/provider_manager.h
    ai/embedding_client.cpp
    ai/embedding_client.h
    ai/sse_parser.cpp
    ai/sse_parser.h
    ai/agent_loop.cpp
    ai/agent_loop.h
    ai/agent_preset.cpp
    ai/agent_preset.h
    ai/preset_store.cpp
    ai/preset_store.h
    ai/agent_worker.cpp
    ai/agent_worker.h

    # ai/tools (M4: agent function calling)
    ai/tools/tool.h
    ai/tools/tool_context.h
    ai/tools/tool_args_util.h
    ai/tools/tool_policy.h
    ai/tools/tool_registry.cpp
    ai/tools/tool_registry.h
    ai/tools/builtin_tools.h
    ai/tools/basic_read_tools.cpp
    ai/tools/static_analysis_tools.cpp
    ai/tools/dynamic_context_tools.cpp
    ai/tools/debug_control_tools.cpp
    ai/tools/debug_write_tools.cpp
    ai/tools/data_write_tools.cpp
    ai/tools/script_tools.cpp
    ai/tools/debug_navigation_tools.cpp
    ai/tools/annotation_tools.cpp
    ai/tools/program_map_tools.cpp
    ai/tools/advanced_bp_tools.cpp
    ai/tools/assembler_pattern_tools.cpp
    ai/tools/cfg_tool.cpp
    ai/tools/patch_misc_tools.cpp
    ai/tools/anti_debug_tools.cpp
    ai/tools/forensic_tools.cpp
    ai/tools/seh_tool.cpp
    ai/tools/injection_stack_tools.cpp
    ai/tools/trace_error_func_tools.cpp

    # debugger
    debugger/disasm_context.cpp
    debugger/disasm_context.h

    # dbg (S2-A 调试器事件总线)
    dbg/event_bus.cpp
    dbg/event_bus.h

    # locator (M3.1 启发式定位器)
    locator/heuristic_hit.h
    locator/api_scanner.cpp
    locator/api_scanner.h
    locator/string_scanner.cpp
    locator/string_scanner.h
    locator/pattern_scanner.cpp
    locator/pattern_scanner.h
    locator/locator_engine.cpp
    locator/locator_engine.h

    # trace (M3.2 调用链追溯 + M3.5 调用栈采样)
    trace/trace_event.h
    trace/trace_recorder.cpp
    trace/trace_recorder.h
    trace/call_graph.cpp
    trace/call_graph.h
    trace/callstack_tracer.cpp
    trace/callstack_tracer.h

    # storage
    storage/session_store.cpp
    storage/session_store.h
    storage/project_context.cpp
    storage/project_context.h
    storage/project_browser.cpp
    storage/project_browser.h
    storage/meta_keys.h

    # ui
    ui/assistant_panel.cpp
    ui/assistant_panel.h
    ui/chat_view.cpp
    ui/chat_view.h
    ui/tool_call_card.cpp
    ui/tool_call_card.h
    ui/history_dialog.cpp
    ui/history_dialog.h
    ui/login_dialog.cpp
    ui/login_dialog.h
    ui/session_list.cpp
    ui/session_list.h
    ui/locator_dialog.cpp
    ui/locator_dialog.h
    ui/preset_editor_dialog.cpp
    ui/preset_editor_dialog.h
    ui/trace_dialog.cpp
    ui/trace_dialog.h
    ui/api_key_dialog.cpp
    ui/api_key_dialog.h
    ui/tool_confirm_dialog.cpp
    ui/tool_confirm_dialog.h
    ui/tools_browser_dialog.cpp
    ui/tools_browser_dialog.h

    # util
    util/logging.cpp
    util/logging.h
    util/paths.cpp
    util/paths.h
    util/config.cpp
    util/config.h
    util/secret_store.cpp
    util/secret_store.h
    util/hashing.cpp
    util/hashing.h
    util/http_options.h

    ${QRC_FILES}
)

add_x64dbg_plugin(${PLUGIN_NAME}
    SOURCES ${PLUGIN_SOURCES}
    LIBS
        Qt5::Core
        Qt5::Gui
        Qt5::Widgets
        Qt5::Network
        Qt5::Concurrent
        cpr::cpr
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        sqlite_vec
        unofficial::sqlite3::sqlite3
        OpenSSL::SSL
        OpenSSL::Crypto
    PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}"
    DEFINES
        PLUGIN_NAME="${PLUGIN_NAME}"
        PLUGIN_VERSION="${PROJECT_VERSION}"
)

# 拷贝插件到方便加载的位置（可选）
set(X64DBG_RELEASE_DIR "E:/gongju/snapshot_2026-04-20_19-04/release"
    CACHE PATH "x64dbg release 目录，用于 install 时直接拷贝插件")

install(TARGETS ${PLUGIN_NAME}
    RUNTIME DESTINATION "${X64DBG_RELEASE_DIR}/${X64DbgSDK_ARCH}/plugins"
)
