// ai/tools/builtin_tools.h
//
// 内置工具注册器：在 ToolRegistry::registerBuiltinTools() 里调用。
// 拆成多个文件实现（basic_read / static_analysis / dynamic / context），
// 但都通过这里的自由函数对外暴露，避免泄露具体 ITool 子类。
#pragma once

namespace x64ai {

class ToolRegistry;

// 在 reg 中注册基础读取工具（M4.4a）：
//   get_disasm / read_memory / read_string / get_registers / list_modules
void registerBasicReadTools(ToolRegistry& reg);

// 在 reg 中注册静态分析工具（M4.4b）：
//   find_xrefs_to / get_function_range / search_pattern
void registerStaticAnalysisTools(ToolRegistry& reg);

// 在 reg 中注册动态 + 上下文工具（M4.4c）：
//   get_callstack / trace_query / locate_api_callers / rag_search
void registerDynamicAndContextTools(ToolRegistry& reg);

// 在 reg 中注册调试控制等待类工具（S2-D）：
//   wait_for_event
void registerDebugControlTools(ToolRegistry& reg);

// 在 reg 中注册写工具（S3-E/F/G）：
//   set_breakpoint / remove_breakpoint / step_in / step_over / run_until / run_dbg_command
// 全部 requiresUserConfirmation()==true + category()==Write
void registerDebugWriteTools(ToolRegistry& reg);

}  // namespace x64ai
