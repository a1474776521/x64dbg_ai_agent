// ai/tools/debug_write_tools.cpp
//
// S3-E/F/G：调试器写工具集合（必须经过用户 confirm + 落 audit）。
//
//   set_breakpoint     在指定 VA 安装软件或硬件断点
//   remove_breakpoint  删除指定 VA 的断点（软件 + 硬件都尝试）
//   step_in            单步进入，并等待暂停（阻塞 ≤ 30s）
//   step_over          单步跳过，并等待暂停（阻塞 ≤ 30s）
//   run_until          下一次性 bp + run + 等暂停 + 删 bp（阻塞 ≤ 30s）
//   run_dbg_command    跑一条 x64dbg 控制台命令；命令首 token 必须在白名单
//
// 共性：
//   - category() = Write
//   - requiresUserConfirmation() = true（dispatch 会走 5s 确认弹窗）
//   - 所有 step/run 类都用 DbgCmdExecDirect + 自己 wait EventBus，
//     不直接 Script::Debug::Wait()（那是简单的 Sleep loop，无 cancel 支持）。
//   - DbgIsDebugging() 检查放最前；进程 running 时拒绝下命令（step 类要求当前是 paused）。
#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"
#include "ai/tools/bp_safety.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>

#include <Windows.h>
#include "bridgemain.h"
#include "_scriptapi_debug.h"

#include "dbg/event_bus.h"
#include "util/logging.h"

namespace x64ai {

namespace {

std::string formatHexU64(std::uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

// 从 args["va"] / "addr" 解析 VA（支持十进制 / 0x hex / number）。
bool parseVa(const nlohmann::json& args, const char* key, std::uint64_t& out, std::string& err)
{
    if (!args.contains(key)) {
        err = std::string("'") + key + "' is required";
        return false;
    }
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

// set_breakpoint 专用：先尝试 parseVa（数字 / hex），失败再走 x64dbg 表达式求值，
// 支持 "kernel32.LoadLibraryW" 这种 module.symbol 语法（见 known-issues K-30）。
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

// 系统 DLL / 高频 API 黑名单 + classifyBpAddr：已抽到共享头 bp_safety.h
// （历史此处和 advanced_bp_tools.cpp 各有一份重复定义；K-32 重构合并）。
// 见 known-issues K-30 / K-32。

// 等下次 Paused/Breakpoint/Stepped；timeoutMs 默认 30s，可被 cancelFlag 提前中断
bool waitForStop(const std::atomic<bool>* cancel, int timeoutMs)
{
    auto& bus = EventBus::instance();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const auto slice    = std::chrono::milliseconds(50);
    DbgEventPayload p{};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel && cancel->load()) return false;
        if (!DbgIsDebugging())        return false;
        if (bus.waitOnce(DbgEvent::Paused,     slice, &p, cancel)) return true;
        if (bus.waitOnce(DbgEvent::Breakpoint, slice, &p, cancel)) return true;
        if (bus.waitOnce(DbgEvent::Stepped,    slice, &p, cancel)) return true;
    }
    return false;
}

// run_dbg_command 白名单 dbgCmdWhitelist() 已抽到 bp_safety.h（K-32 重构）。

std::string toLowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

}  // namespace

// ============= T-01 set_breakpoint =============
class SetBreakpointTool : public ITool {
public:
    std::string name() const override { return "set_breakpoint"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Install a breakpoint at the given address. "
               "'addr' accepts a VA (\"0x401000\", 4198400) or an x64dbg expression "
               "(\"kernel32.LoadLibraryW\", \"user32.MessageBoxW\"). "
               "type=software (default) installs an INT3 breakpoint; "
               "type=hardware installs an HW execute breakpoint (DR0-DR3, max 4). "
               "WARNING: setting an unconditional software breakpoint on hot system APIs "
               "(LoadLibrary, VirtualAlloc, CreateFile, EnterCriticalSection, PeekMessage, ...) "
               "in ntdll/kernel32/user32 etc. will FREEZE the entire OS (mouse/keyboard lag), "
               "because every other process triggers it and the debuggee's UI thread stalls. "
               "For hot APIs, use 'set_conditional_bp' with a filter, or use 'hardware' type, "
               "or break inside the caller in user code instead.";
    }
    std::string descriptionZh() const override
    {
        return "在指定地址安装断点。"
               "'addr' 支持 VA（\"0x401000\"、4198400）或 x64dbg 表达式"
               "（\"kernel32.LoadLibraryW\"、\"user32.MessageBoxW\"）。"
               "type=software（默认）安装 INT3 断点；"
               "type=hardware 安装硬件执行断点（DR0-DR3，总数最多 4 个）。"
               "警告：在 ntdll/kernel32/user32 等系统 DLL 的高频 API "
               "（LoadLibrary、VirtualAlloc、CreateFile、EnterCriticalSection、PeekMessage 等）"
               "上下无条件软断点会冻结整个系统（鼠标键盘严重卡顿），"
               "因为其他进程也会命中，且被调试进程 UI 线程会被反复暂停。"
               "高频 API 应改用 set_conditional_bp（带过滤条件），或用 hardware 类型，"
               "或者在调用方的用户代码里下断点。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"addr", {{"type", "string"},
                          {"description",
                           "VA (\"0x401000\", 4198400) or x64dbg expression "
                           "(\"kernel32.LoadLibraryW\")"}}},
                {"type", {{"type", "string"},
                          {"enum", nlohmann::json::array({"software", "hardware"})},
                          {"description", "Breakpoint type; default 'software'"}}},
            }},
            {"required", nlohmann::json::array({"addr"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        std::uint64_t va = 0; std::string err;
        if (!resolveBpAddr(args, "addr", va, err)) { r.ok = false; r.error = err; return r; }
        std::string bpType = "software";
        if (args.contains("type") && args["type"].is_string()) bpType = args["type"].get<std::string>();

        // 高频系统 API 危险检查：软断点 + 系统模块 + 热 API → 拒绝并给出明确建议
        if (bpType == "software") {
            HotSpotInfo hs = classifyBpAddr(va);
            if (hs.dangerous) {
                r.ok = false;
                r.error =
                    "REFUSED: unconditional software breakpoint on hot system API '" +
                    hs.moduleName + "!" + hs.symbolName + "' (" + formatHexU64(va) + ") "
                    "would freeze the OS (mouse/keyboard lag, debuggee UI thread stalls). "
                    "Use one of: "
                    "(1) set_conditional_bp with a filter expression "
                    "(e.g. break only when arg.get(0) matches a specific value); "
                    "(2) set this breakpoint with type='hardware' (DR0-DR3, only 4 slots, "
                    "still triggers per-thread but cheaper than INT3); "
                    "(3) breakpoint inside the user-code caller of this API instead "
                    "(use find_xrefs_to / locate_api_callers to find call sites in the debuggee).";
                XAI_LOG_WARN("set_breakpoint REFUSED hot API: {}!{} at {}",
                             hs.moduleName, hs.symbolName, formatHexU64(va).c_str());
                return r;
            }
        }

        bool ok;
        if (bpType == "hardware") {
            ok = Script::Debug::SetHardwareBreakpoint(static_cast<duint>(va),
                                                      Script::Debug::HardwareExecute);
        } else if (bpType == "software") {
            ok = Script::Debug::SetBreakpoint(static_cast<duint>(va));
        } else {
            r.ok = false; r.error = "unknown type: " + bpType + " (expected software|hardware)";
            return r;
        }
        if (!ok) {
            r.ok = false;
            r.error = "set breakpoint failed at " + formatHexU64(va) + " (type=" + bpType + ")";
            return r;
        }
        r.ok = true;
        r.data = {{"addr", formatHexU64(va)}, {"type", bpType}, {"installed", true}};

        // 顺手把 module!symbol 回写给 LLM，方便它后续操作（无危险也补充上下文）
        HotSpotInfo hs = classifyBpAddr(va);
        if (!hs.moduleName.empty()) {
            r.data["module"] = hs.moduleName;
            if (!hs.symbolName.empty()) r.data["symbol"] = hs.symbolName;
        }
        return r;
    }
};

// ============= T-02 remove_breakpoint =============
class RemoveBreakpointTool : public ITool {
public:
    std::string name() const override { return "remove_breakpoint"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Remove software AND hardware breakpoint(s) at the given VA. "
               "Returns counts removed of each kind.";
    }
    std::string descriptionZh() const override
    {
        return "删除指定 VA 上的软件断点和硬件断点（两种都清）。返回各类型实际删除的数量。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"addr", {{"type", "string"},
                          {"description", "VA, e.g. \"0x401000\""}}},
            }},
            {"required", nlohmann::json::array({"addr"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        std::uint64_t va = 0; std::string err;
        if (!parseVa(args, "addr", va, err)) { r.ok = false; r.error = err; return r; }

        const bool sw = Script::Debug::DeleteBreakpoint(static_cast<duint>(va));
        const bool hw = Script::Debug::DeleteHardwareBreakpoint(static_cast<duint>(va));
        if (!sw && !hw) {
            r.ok = false;
            r.error = "no breakpoint found at " + formatHexU64(va);
            return r;
        }
        r.ok = true;
        r.data = {
            {"addr",            formatHexU64(va)},
            {"software_removed", sw},
            {"hardware_removed", hw},
        };
        return r;
    }
};

// ============= T-03 step_in =============
class StepInTool : public ITool {
public:
    std::string name() const override { return "step_in"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Single-step into the next instruction (follow CALL). "
               "Blocks until the debuggee pauses again or timeout (default 30s).";
    }
    std::string descriptionZh() const override
    {
        return "单步步入下一条指令（遇到 CALL 会跟进函数）。阻塞至再次暂停或超时（默认 30 秒）。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"timeout_ms", {{"type", "integer"},
                                 {"description", "Optional wait timeout; default 30000, max 60000"}}},
            }},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        int timeoutMs = 30000;
        std::string e;
        if (tryGetInt32Hint(args, "timeout_ms", 100, 60000, timeoutMs, e)) {}
        else if (!e.empty()) { r.ok=false; r.error="invalid 'timeout_ms': "+e; return r; }

        if (!DbgCmdExecDirect("StepInto")) {
            r.ok = false; r.error = "StepInto command failed"; return r;
        }
        if (!waitForStop(ctx.cancelFlag, timeoutMs)) {
            r.ok = false; r.error = "timeout waiting for step to complete"; return r;
        }
        r.ok = true;
        duint cip = DbgValFromString("cip");
        r.data = {{"action", "step_in"}, {"cip", formatHexU64(cip)}};
        return r;
    }
};

// ============= T-04 step_over =============
class StepOverTool : public ITool {
public:
    std::string name() const override { return "step_over"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Single-step over the next instruction (skip into CALL bodies). "
               "Blocks until the debuggee pauses again or timeout (default 30s).";
    }
    std::string descriptionZh() const override
    {
        return "单步步过下一条指令（CALL 不进入子函数体）。阻塞至再次暂停或超时（默认 30 秒）。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"timeout_ms", {{"type", "integer"},
                                 {"description", "Optional wait timeout; default 30000, max 60000"}}},
            }},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        int timeoutMs = 30000;
        std::string e;
        if (tryGetInt32Hint(args, "timeout_ms", 100, 60000, timeoutMs, e)) {}
        else if (!e.empty()) { r.ok=false; r.error="invalid 'timeout_ms': "+e; return r; }

        if (!DbgCmdExecDirect("StepOver")) {
            r.ok = false; r.error = "StepOver command failed"; return r;
        }
        if (!waitForStop(ctx.cancelFlag, timeoutMs)) {
            r.ok = false; r.error = "timeout waiting for step to complete"; return r;
        }
        r.ok = true;
        duint cip = DbgValFromString("cip");
        r.data = {{"action", "step_over"}, {"cip", formatHexU64(cip)}};
        return r;
    }
};

// ============= T-05 run_until =============
class RunUntilTool : public ITool {
public:
    std::string name() const override { return "run_until"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Set a one-shot software breakpoint at the given VA, run the debuggee, "
               "wait for it to hit (timeout 30s), then auto-remove the breakpoint. "
               "If the BP is hit, debuggee remains paused on that instruction. "
               "If timeout, the BP is removed and the debuggee is left in whatever state it is.";
    }
    std::string descriptionZh() const override
    {
        return "在指定 VA 设置一次性软件断点，运行被调试进程，等待命中（超时 30 秒），命中后自动删除该断点。"
               "命中时进程暂停在该指令；超时则同样删除断点，但进程保持当时状态不动。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"addr", {{"type", "string"},
                          {"description", "VA to stop at, e.g. \"0x401234\""}}},
                {"timeout_ms", {{"type", "integer"},
                                 {"description", "Wait timeout; default 30000, max 60000"}}},
            }},
            {"required", nlohmann::json::array({"addr"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        std::uint64_t va = 0; std::string err;
        if (!parseVa(args, "addr", va, err)) { r.ok = false; r.error = err; return r; }
        int timeoutMs = 30000;
        std::string e;
        if (tryGetInt32Hint(args, "timeout_ms", 100, 60000, timeoutMs, e)) {}
        else if (!e.empty()) { r.ok=false; r.error="invalid 'timeout_ms': "+e; return r; }

        // 用 bp <addr>,ss 安装 one-shot；Script API 没有 singleshoot 入口 → 走命令
        char cmd[128];
        std::snprintf(cmd, sizeof(cmd), "bp 0x%llX, ss",
                      static_cast<unsigned long long>(va));
        if (!DbgCmdExecDirect(cmd)) {
            r.ok = false; r.error = "failed to install one-shot bp: " + std::string(cmd);
            return r;
        }
        bool hit = false;
        if (!DbgCmdExecDirect("run")) {
            // 撤销 bp
            (void)Script::Debug::DeleteBreakpoint(static_cast<duint>(va));
            r.ok = false; r.error = "run command failed"; return r;
        }
        hit = waitForStop(ctx.cancelFlag, timeoutMs);
        // 不管 hit 与否都尝试删——singleshoot 命中后 x64dbg 自己也会清，但兜底
        (void)Script::Debug::DeleteBreakpoint(static_cast<duint>(va));

        if (!hit) {
            r.ok = false; r.error = "timeout waiting for run_until target";
            return r;
        }
        r.ok = true;
        duint cip = DbgValFromString("cip");
        r.data = {
            {"action", "run_until"},
            {"target", formatHexU64(va)},
            {"cip",    formatHexU64(cip)},
            {"hit",    cip == static_cast<duint>(va)},
        };
        return r;
    }
};

// ============= run_dbg_command =============
class RunDbgCommandTool : public ITool {
public:
    std::string name() const override { return "run_dbg_command"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Execute a raw x64dbg console command. The first token MUST be in "
               "the plugin whitelist (bp/bpc/bphwc/bpd/bpe, run/StepInto/StepOver/StepOut/pause, "
               "db/dw/dd/dq). Use named tools (set_breakpoint, step_in, ...) whenever possible; "
               "this is an escape hatch for combinations not yet exposed.";
    }
    std::string descriptionZh() const override
    {
        return "执行一条原生 x64dbg 控制台命令。第一个 token 必须在插件白名单内"
               "（bp/bpc/bphwc/bpd/bpe、run/StepInto/StepOver/StepOut/pause、db/dw/dd/dq）。"
               "尽量优先使用具名工具（set_breakpoint、step_in 等）；"
               "本工具是给尚未独立封装的组合命令的逃生口。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"command", {{"type", "string"},
                             {"description", "Full command line, e.g. \"bp 0x401000, ss\""}}},
            }},
            {"required", nlohmann::json::array({"command"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        if (!args.contains("command") || !args["command"].is_string()) {
            r.ok = false; r.error = "'command' required (string)"; return r;
        }
        std::string cmd = args["command"].get<std::string>();
        if (cmd.empty() || cmd.size() > 512) {
            r.ok = false; r.error = "'command' length out of range [1, 512]"; return r;
        }
        // 取首 token（按空白切分）
        std::string head;
        for (char c : cmd) {
            if (c == ' ' || c == '\t' || c == ',') break;
            head.push_back(c);
        }
        const std::string headLower = toLowerCopy(head);
        const auto& wl = dbgCmdWhitelist();
        if (wl.find(headLower) == wl.end()) {
            r.ok = false;
            r.error = "command '" + head + "' not in whitelist";
            XAI_LOG_WARN("run_dbg_command: blocked '{}' (head '{}')", cmd, head);
            return r;
        }
        if (!DbgCmdExecDirect(cmd.c_str())) {
            r.ok = false; r.error = "DbgCmdExecDirect failed for: " + cmd; return r;
        }
        r.ok = true;
        r.data = {{"command", cmd}, {"executed", true}};
        return r;
    }
};

// ============= 会话控制：start / attach / detach / restart / stop =============
//
// 这五个都是高危：直接管控 x64dbg 调试会话生命周期。
//   - start/attach 会拉起 / 接管进程；
//   - restart 会杀掉当前调试进程并重启；
//   - detach 会让进程脱离调试器（继续裸跑）；
//   - stop 会杀掉当前进程。
// 都走 requiresUserConfirmation=true，依靠 ToolRegistry::dispatch 弹 5s 倒计时。
//
// 命令对应（见 x64dbg 文档 commands/debug-control/index.html）：
//   InitDebug "target.exe"   启动新调试（也有别名 init / initdbg）
//   attach    pid             附加到 pid
//   detach                    脱离当前调试
//   Restart                   重启当前调试（保留断点）
//   StopDebug                 结束当前调试（也有别名 stop）

class StartDebugTool : public ITool {
public:
    std::string name() const override { return "start_debug"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Start a new debug session by launching 'target_path'. Optionally pass "
               "'command_line' as additional CLI arguments to the target. Fails if a "
               "session is already active - call stop_debug or restart_debug first. "
               "DANGEROUS: launches a process. Requires user confirmation.";
    }
    std::string descriptionZh() const override
    {
        return "启动新调试会话：拉起 'target_path' 指定的程序。可选传 'command_line' "
               "作为目标程序的附加命令行参数。如已存在调试会话会失败 —— 先调 stop_debug "
               "或 restart_debug。危险操作：会启动一个新进程，需要用户确认。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"target_path",  {{"type","string"},
                                  {"description","Absolute path to the .exe/.dll to debug"}}},
                {"command_line", {{"type","string"},
                                  {"description","Optional CLI args passed to the target"}}},
            }},
            {"required", nlohmann::json::array({"target_path"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        if (DbgIsDebugging()) {
            r.ok=false;
            r.error="a debug session is already active - call stop_debug or restart_debug first";
            return r;
        }
        if (!args.contains("target_path") || !args["target_path"].is_string()) {
            r.ok=false; r.error="'target_path' required (string)"; return r;
        }
        std::string target = args["target_path"].get<std::string>();
        if (target.empty() || target.size() > 1024) {
            r.ok=false; r.error="'target_path' length out of range [1, 1024]"; return r;
        }
        // x64dbg InitDebug 语法：路径不带引号会被空格切断；统一加引号
        // 文档：InitDebug "C:\path\to\target.exe" [cmdline]
        std::string cmd = "InitDebug \"" + target + "\"";
        if (args.contains("command_line") && args["command_line"].is_string()) {
            const auto cl = args["command_line"].get<std::string>();
            if (!cl.empty()) cmd += ", " + cl;  // x64dbg InitDebug 第二参数也用逗号
        }
        XAI_LOG_INFO("start_debug: cmd='{}'", cmd.c_str());
        if (!DbgCmdExecDirect(cmd.c_str())) {
            r.ok=false; r.error="DbgCmdExecDirect failed for: " + cmd;
            return r;
        }
        r.ok=true;
        r.data = {
            {"command", cmd},
            {"target_path", target},
            {"note", "session launch issued; debugger will reach system breakpoint asynchronously - "
                     "use wait_for_event to observe initial pause"},
        };
        return r;
    }
};

class AttachDebugTool : public ITool {
public:
    std::string name() const override { return "attach_debug"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Attach the debugger to a running process by PID. Fails if a session "
               "is already active. DANGEROUS: intercepts another process; requires "
               "user confirmation.";
    }
    std::string descriptionZh() const override
    {
        return "通过 PID 附加到一个正在运行的进程。如已存在调试会话会失败。"
               "危险操作：会接管另一个进程，需要用户确认。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"pid", {{"type","integer"},{"minimum",1},
                         {"description","Target process ID (decimal)"}}},
            }},
            {"required", nlohmann::json::array({"pid"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        if (DbgIsDebugging()) {
            r.ok=false;
            r.error="a debug session is already active - call detach or stop_debug first";
            return r;
        }
        if (!args.contains("pid") || !args["pid"].is_number_integer()) {
            r.ok=false; r.error="'pid' required (positive integer)"; return r;
        }
        const long long pid = args["pid"].get<long long>();
        if (pid <= 0 || pid > 0xFFFFFFFFLL) {
            r.ok=false; r.error="'pid' out of range"; return r;
        }
        // x64dbg attach 命令接收十六进制 PID（按文档默认所有数值都按 hex 解析）
        char buf[64];
        std::snprintf(buf, sizeof(buf), "attach %llx", pid);
        XAI_LOG_INFO("attach_debug: cmd='{}' pid={}", buf, pid);
        if (!DbgCmdExecDirect(buf)) {
            r.ok=false; r.error=std::string("DbgCmdExecDirect failed for: ") + buf;
            return r;
        }
        r.ok=true;
        r.data = {{"command", buf}, {"pid", pid}};
        return r;
    }
};

class DetachDebugTool : public ITool {
public:
    std::string name() const override { return "detach_debug"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Detach the debugger from the current process; the process continues "
               "running without debugging. Fails if no active session. Requires user confirmation.";
    }
    std::string descriptionZh() const override
    {
        return "脱离当前调试进程：进程会继续运行，不再被调试。无活动会话时失败。需要用户确认。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {{"type","object"},{"properties", nlohmann::json::object()}};
    }
    ToolResult invoke(const nlohmann::json& /*args*/, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        if (!DbgIsDebugging()) { r.ok=false; r.error="no active debug session"; return r; }
        XAI_LOG_INFO("detach_debug invoked");
        if (!DbgCmdExecDirect("detach")) {
            r.ok=false; r.error="DbgCmdExecDirect failed for: detach"; return r;
        }
        r.ok=true; r.data={{"command","detach"}};
        return r;
    }
};

class RestartDebugTool : public ITool {
public:
    std::string name() const override { return "restart_debug"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Restart the current debug session: terminate the debuggee, then re-launch "
               "it with the same target/cmdline. Breakpoints set in x64dbg are preserved. "
               "Fails if no active session - use start_debug instead. DANGEROUS: kills the "
               "currently debugged process; requires user confirmation.";
    }
    std::string descriptionZh() const override
    {
        return "重启当前调试会话：先终止被调试进程，再用原 target/命令行重新拉起。"
               "x64dbg 里设置的断点会被保留。无活动会话时失败 —— 改用 start_debug。"
               "危险操作：会杀掉当前被调试进程，需要用户确认。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {{"type","object"},{"properties", nlohmann::json::object()}};
    }
    ToolResult invoke(const nlohmann::json& /*args*/, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        if (!DbgIsDebugging()) {
            r.ok=false;
            r.error="no active debug session - call start_debug to launch a new one";
            return r;
        }
        XAI_LOG_INFO("restart_debug invoked");
        if (!DbgCmdExecDirect("Restart")) {
            r.ok=false; r.error="DbgCmdExecDirect failed for: Restart"; return r;
        }
        r.ok=true;
        r.data = {
            {"command","Restart"},
            {"note","debuggee terminated and re-launched asynchronously; use wait_for_event "
                    "to observe initial system breakpoint"},
        };
        return r;
    }
};

class StopDebugTool : public ITool {
public:
    std::string name() const override { return "stop_debug"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Stop the current debug session: terminate the debuggee. No-op if no "
               "active session. DANGEROUS: kills the debugged process; requires user confirmation.";
    }
    std::string descriptionZh() const override
    {
        return "结束当前调试会话：终止被调试进程。无活动会话时返回 no-op。"
               "危险操作：会杀掉被调试进程，需要用户确认。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {{"type","object"},{"properties", nlohmann::json::object()}};
    }
    ToolResult invoke(const nlohmann::json& /*args*/, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        if (!DbgIsDebugging()) {
            r.ok=true;
            r.data = {{"command","StopDebug"},{"note","no active session, no-op"}};
            return r;
        }
        XAI_LOG_INFO("stop_debug invoked");
        if (!DbgCmdExecDirect("StopDebug")) {
            r.ok=false; r.error="DbgCmdExecDirect failed for: StopDebug"; return r;
        }
        r.ok=true; r.data = {{"command","StopDebug"}};
        return r;
    }
};

void registerDebugWriteTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<SetBreakpointTool>(), "breakpoint");
    reg.registerTool(std::make_unique<RemoveBreakpointTool>(), "breakpoint");
    reg.registerTool(std::make_unique<StepInTool>(), "execution-control");
    reg.registerTool(std::make_unique<StepOverTool>(), "execution-control");
    reg.registerTool(std::make_unique<RunUntilTool>(), "execution-control");
    reg.registerTool(std::make_unique<RunDbgCommandTool>(), "write-patch");
    reg.registerTool(std::make_unique<StartDebugTool>(), "session-control");
    reg.registerTool(std::make_unique<AttachDebugTool>(), "session-control");
    reg.registerTool(std::make_unique<DetachDebugTool>(), "session-control");
    reg.registerTool(std::make_unique<RestartDebugTool>(), "session-control");
    reg.registerTool(std::make_unique<StopDebugTool>(), "session-control");
}

}  // namespace x64ai
