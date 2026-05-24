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

// 在 reg 中注册数据写工具（S4 / T-07..T-09）：
//   patch_memory / set_register / write_string
// 全部 requiresUserConfirmation()==true + category()==Write
void registerDataWriteTools(ToolRegistry& reg);

// 在 reg 中注册脚本工具（S5 / W-1..W-3）：
//   list_scripts (Read) / load_script (Write) / run_script_file (Write)
void registerScriptTools(ToolRegistry& reg);

// 在 reg 中注册调试导航工具（S6-A）：
//   run_continue / pause_debug / step_out
// run_continue 默认 fire-and-forget；可选 wait_for_stop=true 阻塞至 Paused
void registerDebugNavigationTools(ToolRegistry& reg);

// 在 reg 中注册标签/注释工具（S6-B + S6-C）：
//   set_label / get_label / list_labels
//   set_comment / get_comment / list_comments
// set_* 是 Write+confirm；text="" 即删除。
void registerAnnotationTools(ToolRegistry& reg);

// 在 reg 中注册程序地图与内存映射工具（S6-D + S6-E）：
//   get_memory_map / get_page_protect / set_page_protect (Write+confirm)
//   list_functions / get_module_imports / get_module_exports
void registerProgramMapTools(ToolRegistry& reg);

// 在 reg 中注册高级断点工具（S7-A + S7-B）：
//   set_hw_breakpoint / remove_hw_breakpoint (Write+confirm)
//   set_conditional_bp (Write+confirm，基于 BpRefVa + BpSetFieldText)
void registerAdvancedBpTools(ToolRegistry& reg);

// 在 reg 中注册汇编 / 模式 / 标志位工具（S7-C + S7-D + S7-F）：
//   assemble_at (Write+confirm，AssembleMemEx + fill_nop)
//   pattern_replace (Write+confirm，SearchAndReplaceMem，size<=16MB)
//   set_flag (Write+confirm，ZF/OF/CF/PF/SF/TF/AF/DF/IF)
void registerAssemblerPatternTools(ToolRegistry& reg);

// 在 reg 中注册 CFG 工具（S7-E）：
//   get_cfg (Read，DbgAnalyzeFunction + BridgeCFGraph → Mermaid graph TD)
void registerCfgTools(ToolRegistry& reg);

// 在 reg 中注册补丁审计 / 模板 / GUI 焦点工具（S7-G + S7-H + S7-I）：
//   list_patches (Read) / restore_patch (Write+confirm)
//   format_with_dbg (Read，StringFormatInline)
//   gui_focus_disasm / gui_focus_dump (DbgControl，无副作用)
void registerPatchMiscTools(ToolRegistry& reg);

}  // namespace x64ai
