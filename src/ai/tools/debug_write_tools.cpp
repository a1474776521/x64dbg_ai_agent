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

// run_dbg_command 白名单（首 token，大小写不敏感）。
// 注意：x64dbg 命令大小写不敏感且有大量缩写，这里只列**完整 token**。
const std::unordered_set<std::string>& dbgCmdWhitelist()
{
    static const std::unordered_set<std::string> wl = {
        // 断点
        "bp", "bpc", "bphwc", "bpd", "bpe",
        // 执行控制
        "run", "stepinto", "stepover", "stepout", "pause",
        // 数据读取（虽然有专门 read_memory，但允许 LLM 用 dump 命令偶尔查）
        "db", "dw", "dd", "dq",
    };
    return wl;
}

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
        return "Install a breakpoint at the given VA. "
               "type=software (default) installs a soft INT3 breakpoint; "
               "type=hardware installs an HW execute breakpoint (DR0-DR3, limited to 4). "
               "Use this when you need the debuggee to stop at a specific address.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"addr", {{"type", "string"},
                          {"description", "VA, e.g. \"0x401000\" or 4198400"}}},
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
        if (!parseVa(args, "addr", va, err)) { r.ok = false; r.error = err; return r; }
        std::string bpType = "software";
        if (args.contains("type") && args["type"].is_string()) bpType = args["type"].get<std::string>();

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

void registerDebugWriteTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<SetBreakpointTool>());
    reg.registerTool(std::make_unique<RemoveBreakpointTool>());
    reg.registerTool(std::make_unique<StepInTool>());
    reg.registerTool(std::make_unique<StepOverTool>());
    reg.registerTool(std::make_unique<RunUntilTool>());
    reg.registerTool(std::make_unique<RunDbgCommandTool>());
}

}  // namespace x64ai
