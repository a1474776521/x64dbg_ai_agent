// ai/tools/advanced_bp_tools.cpp
//
// S7-A 硬件断点：set_hw_breakpoint / remove_hw_breakpoint
// S7-B 条件/log/命令断点：set_conditional_bp
//
// 硬件断点：
//   - x64dbg 把 DR7 的 LEN 字段封到内部，脚本层只允许指定 type（execute/write/access）。
//   - DR0~DR3 共 4 槽，超限 SDK 直接返 false。
//   - x86 下 DR 寄存器仍是 32-bit，address 自然窄；x64 是 64-bit。
//
// 条件断点：
//   - 通过 BP_REF + BpSetFieldText 改字段。先 BpRefVa 解出 ref，再设各字段。
//   - 支持 condition / logText / logCondition / command / commandCondition / fastResume / silent。
//   - 任意字段缺省 = 不动；如果要清空已有字段，传空串 "" 即可（底层就是用空串清字段）。
#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"
#include "ai/tools/bp_safety.h"

#include <cstdio>
#include <string>
#include <unordered_set>
#include <algorithm>

#include <Windows.h>
#include "bridgemain.h"
#include "_dbgfunctions.h"
#include "_scriptapi_debug.h"

#include "util/logging.h"

namespace x64ai {

namespace {

std::string formatHexU64(std::uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

bool parseVa(const nlohmann::json& args, const char* key, std::uint64_t& out, std::string& err)
{
    if (!args.contains(key)) { err = std::string("'") + key + "' is required"; return false; }
    const auto& v = args[key];
    if (v.is_number_unsigned()) { out = v.get<std::uint64_t>(); return true; }
    if (v.is_number_integer())  { out = static_cast<std::uint64_t>(v.get<std::int64_t>()); return true; }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        try {
            std::size_t pos = 0;
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                out = std::stoull(s.substr(2), &pos, 16);
                if (pos + 2 == s.size()) return true;
            } else {
                out = std::stoull(s, &pos, 0);
                if (pos == s.size()) return true;
            }
        } catch (...) {}
    }
    err = std::string("invalid '") + key + "': expected number or hex string";
    return false;
}

// 同 debug_write_tools.cpp::resolveBpAddr：支持 "kernel32.LoadLibraryW" 这种 module.symbol 语法
// 见 known-issues K-30。
bool resolveBpAddr(const nlohmann::json& args, const char* key,
                   std::uint64_t& out, std::string& err)
{
    if (parseVa(args, key, out, err)) { err.clear(); return true; }
    if (!args.contains(key) || !args[key].is_string()) return false;
    const std::string expr = args[key].get<std::string>();
    if (expr.empty()) { err = "empty address expression"; return false; }
    bool ok = false;
    duint v = DbgEval(expr.c_str(), &ok);
    if (!ok || v == 0) {
        err = "failed to resolve address: '" + expr +
              "' (not a number/hex and not a valid x64dbg expression; "
              "try eval_expression first, or check module is loaded)";
        return false;
    }
    out = static_cast<std::uint64_t>(v);
    err.clear();
    return true;
}

// 系统模块 / 高频 API 黑名单 + classifyBpAddr：已抽到共享头 bp_safety.h（K-32 重构）。
// 见 known-issues K-30 / K-32。

}  // namespace

// ============= S7-A set_hw_breakpoint =============
class SetHwBreakpointTool : public ITool {
public:
    std::string name() const override { return "set_hw_breakpoint"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Set a hardware breakpoint. 'address' accepts a VA or an x64dbg expression "
               "(\"kernel32.LoadLibraryW\"). type ∈ {execute,write,access}. "
               "Only 4 HW BPs total (DR0-DR3); over-limit silently fails. "
               "WARNING: hardware breakpoints on hot system APIs in ntdll/kernel32/user32 "
               "(LoadLibrary, VirtualAlloc, CreateFile, EnterCriticalSection, PeekMessage, ...) "
               "will FREEZE the OS, because the debuggee gets paused hundreds of times per second "
               "and if it holds global hooks (mouse/keyboard hooks, anti-cheat hooks), "
               "the whole desktop becomes unresponsive. HW BPs are NOT a safe alternative to "
               "soft BPs for hot APIs - use 'set_conditional_bp' with a filter, or break inside "
               "the user-code caller (find_xrefs_to / locate_api_callers) instead.";
    }
    std::string descriptionZh() const override
    {
        return "设置硬件断点。'address' 支持 VA 或 x64dbg 表达式（\"kernel32.LoadLibraryW\"）。"
               "type ∈ {execute=执行 / write=写入 / access=访问}。"
               "总共最多 4 个硬件断点（DR0-DR3），超限会静默失败。"
               "警告：在 ntdll/kernel32/user32 等系统 DLL 的高频 API "
               "（LoadLibrary、VirtualAlloc、CreateFile、EnterCriticalSection、PeekMessage 等）"
               "上下硬件断点也会冻结系统，因为被调试进程每秒被暂停几百次，"
               "如果它持有全局键鼠 hook 或反作弊 hook，整个桌面都会卡死。"
               "对高频 API 来说硬件断点并不是软件断点的安全替代 —— 必须改用 "
               "set_conditional_bp（带过滤条件），或在调用方的用户代码里下断（find_xrefs_to / locate_api_callers）。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"address", {{"type","string"},
                             {"description","VA (\"0x401000\") or x64dbg expression (\"kernel32.LoadLibraryW\")"}}},
                {"type",    {{"type","string"},{"enum", nlohmann::json::array({"execute","write","access"})},
                             {"description","Trigger type; default execute"}}},
            }},
            {"required", nlohmann::json::array({"address"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        std::uint64_t va = 0; std::string e;
        if (!resolveBpAddr(args, "address", va, e)) { r.ok=false; r.error=e; return r; }
        Script::Debug::HardwareType t = Script::Debug::HardwareExecute;
        std::string typeStr = "execute";
        if (args.contains("type") && args["type"].is_string()) {
            typeStr = args["type"].get<std::string>();
            if      (typeStr == "execute") t = Script::Debug::HardwareExecute;
            else if (typeStr == "write")   t = Script::Debug::HardwareWrite;
            else if (typeStr == "access")  t = Script::Debug::HardwareAccess;
            else { r.ok=false; r.error="invalid 'type': use execute/write/access"; return r; }
        }

        // 高频系统 API 防护：execute 类型 + 系统模块 + 热 API → 拒绝
        // write/access 是数据断点，对系统 API 入口下数据断点很少见，且语义不同，放行。
        if (t == Script::Debug::HardwareExecute) {
            HotSpotInfo hs = classifyBpAddr(va);
            if (hs.dangerous) {
                r.ok = false;
                r.error =
                    "REFUSED: hardware execute breakpoint on hot system API '" +
                    hs.moduleName + "!" + hs.symbolName + "' (" + formatHexU64(va) + ") "
                    "will freeze the OS - the debuggee is paused hundreds of times per second, "
                    "and if it holds any global hook (mouse/keyboard/anti-cheat) the whole desktop "
                    "becomes unresponsive (proven by 17:03-17:04 log run_continue timeout). "
                    "Hardware BP is NOT a safe escape from the soft-BP refusal. Use: "
                    "(1) set_conditional_bp with a filter on arg.get(0) etc. so only matching "
                    "calls actually pause; "
                    "(2) breakpoint inside the user-code caller of this API "
                    "(call find_xrefs_to / locate_api_callers in the debuggee module first).";
                XAI_LOG_WARN("set_hw_breakpoint REFUSED hot API: {}!{} at {}",
                             hs.moduleName, hs.symbolName, formatHexU64(va).c_str());
                return r;
            }
        }

        const bool ok = Script::Debug::SetHardwareBreakpoint(static_cast<duint>(va), t);
        if (!ok) {
            r.ok=false;
            r.error="Script::Debug::SetHardwareBreakpoint failed at " + formatHexU64(va) +
                    " (possible cause: 4 HW BP slots exhausted)";
            return r;
        }
        XAI_LOG_INFO("set_hw_breakpoint: va={} type={}", formatHexU64(va).c_str(), typeStr.c_str());
        r.ok=true;
        r.data = {
            {"address", formatHexU64(va)},
            {"type",    typeStr},
        };
        HotSpotInfo hs = classifyBpAddr(va);
        if (!hs.moduleName.empty()) {
            r.data["module"] = hs.moduleName;
            if (!hs.symbolName.empty()) r.data["symbol"] = hs.symbolName;
        }
        return r;
    }
};

// ============= S7-A remove_hw_breakpoint =============
class RemoveHwBreakpointTool : public ITool {
public:
    std::string name() const override { return "remove_hw_breakpoint"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Delete a hardware breakpoint at address. No-op if none exists at that VA.";
    }
    std::string descriptionZh() const override
    {
        return "删除指定地址的硬件断点。该地址若无硬件断点则不做任何事。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"address", {{"type","string"},{"description","VA, decimal or 0x hex"}}},
            }},
            {"required", nlohmann::json::array({"address"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        std::uint64_t va = 0; std::string e;
        if (!parseVa(args, "address", va, e)) { r.ok=false; r.error=e; return r; }
        const bool ok = Script::Debug::DeleteHardwareBreakpoint(static_cast<duint>(va));
        r.ok=true;  // 不存在也不报错（Read-after-write 语义）
        r.data = {{"address", formatHexU64(va)}, {"removed", ok}};
        return r;
    }
};

// ============= S7-B set_conditional_bp =============
class SetConditionalBpTool : public ITool {
public:
    std::string name() const override { return "set_conditional_bp"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Configure software-BP fields at an existing BP: break/log/command + their conditions, "
               "fastResume, silent. The BP MUST exist (call set_breakpoint first). "
               "Empty string ('') clears a text field. Numeric expressions are x64dbg expressions, "
               "e.g. condition='rax==0x42'.";
    }
    std::string descriptionZh() const override
    {
        return "配置已存在软件断点的高级字段：break/log/command 条件、fastResume、silent。"
               "断点必须已存在（先调用 set_breakpoint）。空串 '' 表示清除该文本字段。"
               "条件表达式按 x64dbg 表达式语法，例如 condition='rax==0x42'。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"address",         {{"type","string"},{"description","VA of an existing software BP"}}},
                {"condition",       {{"type","string"},{"description","Break condition (x64dbg expr)"}}},
                {"logText",         {{"type","string"},{"description","Text to log on hit; supports {...} format"}}},
                {"logCondition",    {{"type","string"},{"description","Condition for logging only"}}},
                {"command",         {{"type","string"},{"description","x64dbg command to run on hit"}}},
                {"commandCondition",{{"type","string"},{"description","Condition for command only"}}},
                {"fastResume",      {{"type","boolean"},{"description","Auto-resume without breaking GUI"}}},
                {"silent",          {{"type","boolean"},{"description","Suppress log line for the break itself"}}},
                {"name",            {{"type","string"},{"description","Optional name shown in BP list"}}},
            }},
            {"required", nlohmann::json::array({"address"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        std::uint64_t va = 0; std::string e;
        if (!parseVa(args, "address", va, e)) { r.ok=false; r.error=e; return r; }
        const auto* fns = DbgFunctions();
        if (!fns || !fns->BpRefVa || !fns->BpSetFieldText || !fns->BpSetFieldNumber || !fns->BpRefExists) {
            r.ok=false; r.error="required DbgFunctions slots are null (x64dbg too old?)"; return r;
        }
        BP_REF ref{};
        if (!fns->BpRefVa(&ref, bp_normal, static_cast<duint>(va))) {
            r.ok=false; r.error="no software BP exists at " + formatHexU64(va) + " (call set_breakpoint first)";
            return r;
        }
        if (!fns->BpRefExists(&ref)) {
            r.ok=false; r.error="BP_REF resolved but BP no longer exists at " + formatHexU64(va);
            return r;
        }

        nlohmann::json applied = nlohmann::json::object();

        auto setText = [&](const char* key, BP_FIELD field) -> bool {
            if (!args.contains(key) || !args[key].is_string()) return true;  // 不传 = 不动
            const auto v = args[key].get<std::string>();
            if (!fns->BpSetFieldText(&ref, field, v.c_str())) return false;
            applied[key] = v;
            return true;
        };
        auto setBool = [&](const char* key, BP_FIELD field) -> bool {
            if (!args.contains(key) || !args[key].is_boolean()) return true;
            const bool v = args[key].get<bool>();
            if (!fns->BpSetFieldNumber(&ref, field, v ? 1 : 0)) return false;
            applied[key] = v;
            return true;
        };

        if (!setText("condition",        bpf_breakcondition))   { r.ok=false; r.error="BpSetFieldText(breakcondition) failed";   return r; }
        if (!setText("logText",          bpf_logtext))          { r.ok=false; r.error="BpSetFieldText(logtext) failed";          return r; }
        if (!setText("logCondition",     bpf_logcondition))     { r.ok=false; r.error="BpSetFieldText(logcondition) failed";     return r; }
        if (!setText("command",          bpf_commandtext))      { r.ok=false; r.error="BpSetFieldText(commandtext) failed";      return r; }
        if (!setText("commandCondition", bpf_commandcondition)) { r.ok=false; r.error="BpSetFieldText(commandcondition) failed"; return r; }
        if (!setText("name",             bpf_name))             { r.ok=false; r.error="BpSetFieldText(name) failed";             return r; }
        if (!setBool("fastResume",       bpf_fastresume))       { r.ok=false; r.error="BpSetFieldNumber(fastresume) failed";     return r; }
        if (!setBool("silent",           bpf_silent))           { r.ok=false; r.error="BpSetFieldNumber(silent) failed";         return r; }

        XAI_LOG_INFO("set_conditional_bp: va={} applied={}", formatHexU64(va).c_str(), applied.dump().c_str());
        r.ok=true;
        r.data = {{"address", formatHexU64(va)}, {"applied", std::move(applied)}};
        return r;
    }
};

void registerAdvancedBpTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<SetHwBreakpointTool>(), "breakpoint");
    reg.registerTool(std::make_unique<RemoveHwBreakpointTool>(), "breakpoint");
    reg.registerTool(std::make_unique<SetConditionalBpTool>(), "breakpoint");
}

}  // namespace x64ai
