// ai/tools/debug_navigation_tools.cpp
//
// S6-A：调试导航控制三件套。
//
//   run_continue   继续运行（解决 S3 "没有 run" 的缺口；之前 agent 只能走 run_dbg_command）
//   pause_debug    异步打断长循环
//   step_out       跳出当前函数
//
// 共性：
//   - category() = Write
//   - requiresUserConfirmation() = true（5s confirm + audit，与 S3 一致）
//   - 与 S3 step_in/step_over 同源等待：用 EventBus 阻塞等 Paused/Breakpoint/Stepped。
//   - run_continue 默认不等待返回（fire-and-forget）；调用方应配合 wait_for_event 观察。
//     如果用户要"运行直到下一次自动停"，传 wait_for_stop=true，但仍受 timeout 保护。
//   - pause_debug 同步等 Paused（pause 命令本身很快出 Paused 事件）。
#include "ai/tools/builtin_tools.h"
#include "ai/tools/dbg_state_util.h"  // K-43: 公共 currentDbgStateStr / currentCipHexOrEmpty
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

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

// K-42: x64dbg debuggee 运行态字符串。
//   "not_debugging" : 未附加任何进程（DbgIsDebugging=false）
//   "running"       : 已附加且 debuggee 正在跑（DbgIsRunning=true）
//   "paused"        : 已附加但 debuggee 停下（断点/暂停/单步结束等）
// 用途：执行控制类工具的前置校验 + 错误信息里带回当前状态，
//       让 LLM 看到失败原因后能自我纠正（不必再去查 get_debug_state）。
// K-43: 抽到 dbg_state_util.h 作为公共 helper；本文件保留 using 引用以便最小改动。
using x64ai::currentDbgStateStr;

}  // namespace

// ============= T-N1 run_continue =============
class RunContinueTool : public ITool {
public:
    std::string name() const override { return "run_continue"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Continue execution (Run). Default: fire-and-forget, returns immediately - "
               "pair with wait_for_event to observe the next stop. "
               "Set wait_for_stop=true to block here until Paused/Breakpoint (timeout default 30s).";
    }
    std::string descriptionZh() const override
    {
        return "继续执行（Run）。默认 fire-and-forget 立刻返回 —— 之后用 wait_for_event 观察下次暂停。"
               "若设 wait_for_stop=true 则阻塞至 Paused/Breakpoint（超时默认 30 秒、上限 300 秒）。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"wait_for_stop", {{"type", "boolean"},
                                   {"description", "Block until next stop; default false"}}},
                {"timeout_ms",    {{"type", "integer"},
                                   {"description", "Wait timeout when wait_for_stop=true; default 30000, max 300000 (5 min, raised in K-37 for unpack/trace scenarios)"}}},
            }},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        // K-42: 状态前置校验。x64dbg cbDebugRun 在 debuggee 已 running 时直接 return false
        // ("Program is already running")，DbgCmdExecDirect 因此返回 false，旧错误信息
        // "run command failed" 对 LLM 完全没指导价值；这里直接拒并带回当前状态，提示
        // 它走 wait_for_event 或 pause_debug。
        if (DbgIsRunning()) {
            r.ok = false;
            r.error = "cannot continue: debuggee is currently 'running'; "
                      "call wait_for_event (to observe the next stop) or "
                      "pause_debug (to break in) first";
            r.data = {{"current_state", "running"}};
            return r;
        }
        bool wait = false;
        if (args.contains("wait_for_stop") && args["wait_for_stop"].is_boolean()) {
            wait = args["wait_for_stop"].get<bool>();
        }
        int timeoutMs = 30000;
        std::string e;
        if (tryGetInt32Hint(args, "timeout_ms", 100, 300000, timeoutMs, e)) {}
        else if (!e.empty()) { r.ok=false; r.error="invalid 'timeout_ms': "+e; return r; }

        if (!DbgCmdExecDirect("run")) {
            r.ok = false;
            r.error = std::string("run command rejected by x64dbg (current_state=")
                      + currentDbgStateStr() + ")";
            return r;
        }
        if (!wait) {
            r.ok = true;
            r.data = {{"action","run_continue"},{"waited",false},
                      {"note","fire-and-forget; use wait_for_event for next stop"}};
            return r;
        }
        if (!waitForStop(ctx.cancelFlag, timeoutMs)) {
            r.ok = false; r.error = "timeout waiting for next stop";
            return r;
        }
        r.ok = true;
        duint cip = DbgValFromString("cip");
        r.data = {{"action","run_continue"},{"waited",true},{"cip", formatHexU64(cip)}};
        return r;
    }
};

// ============= T-N2 pause_debug =============
class PauseDebugTool : public ITool {
public:
    std::string name() const override { return "pause_debug"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Pause the running debuggee (asynchronous interrupt). "
               "Blocks until Paused event or timeout (default 5s).";
    }
    std::string descriptionZh() const override
    {
        return "暂停正在运行的被调试进程（异步中断）。阻塞至 Paused 事件或超时（默认 5 秒）。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"timeout_ms", {{"type", "integer"},
                                {"description", "Wait timeout; default 5000, max 30000"}}},
            }},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        // K-42: x64dbg cbDebugPause 要求 dbgisrunning()，否则 return false。
        // 已 paused 时拒并附 cip，提示 agent 已经停了不必再 pause。
        if (!DbgIsRunning()) {
            duint cip = DbgValFromString("cip");
            r.ok = false;
            r.error = "cannot pause: debuggee is already 'paused' at "
                      + formatHexU64(cip)
                      + "; if you need to read state, just call get_registers / get_disasm";
            r.data = {{"current_state", "paused"}, {"cip", formatHexU64(cip)}};
            return r;
        }
        int timeoutMs = 5000;
        std::string e;
        if (tryGetInt32Hint(args, "timeout_ms", 100, 30000, timeoutMs, e)) {}
        else if (!e.empty()) { r.ok=false; r.error="invalid 'timeout_ms': "+e; return r; }

        if (!DbgCmdExecDirect("pause")) {
            r.ok = false;
            r.error = std::string("pause command rejected by x64dbg (current_state=")
                      + currentDbgStateStr() + ")";
            return r;
        }
        if (!waitForStop(ctx.cancelFlag, timeoutMs)) {
            r.ok = false; r.error = "timeout waiting for pause to take effect";
            return r;
        }
        r.ok = true;
        duint cip = DbgValFromString("cip");
        r.data = {{"action","pause_debug"},{"cip", formatHexU64(cip)}};
        return r;
    }
};

// ============= T-N3 step_out =============
class StepOutTool : public ITool {
public:
    std::string name() const override { return "step_out"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Step out of the current function (until RET executes and control returns to caller). "
               "Blocks until next pause or timeout (default 30s).";
    }
    std::string descriptionZh() const override
    {
        return "跳出当前函数（执行到 RET 返回调用者）。阻塞至下次暂停或超时（默认 30 秒）。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"timeout_ms", {{"type", "integer"},
                                {"description", "Wait timeout; default 30000, max 60000"}}},
            }},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        // K-42: StepOut 要求 paused 状态；running 时 x64dbg 拒收。
        if (DbgIsRunning()) {
            r.ok = false;
            r.error = "cannot step_out: debuggee is currently 'running'; "
                      "call wait_for_event or pause_debug first to reach a paused state";
            r.data = {{"current_state", "running"}};
            return r;
        }
        int timeoutMs = 30000;
        std::string e;
        if (tryGetInt32Hint(args, "timeout_ms", 100, 60000, timeoutMs, e)) {}
        else if (!e.empty()) { r.ok=false; r.error="invalid 'timeout_ms': "+e; return r; }

        if (!DbgCmdExecDirect("StepOut")) {
            r.ok = false;
            r.error = std::string("StepOut command rejected by x64dbg (current_state=")
                      + currentDbgStateStr() + ")";
            return r;
        }
        if (!waitForStop(ctx.cancelFlag, timeoutMs)) {
            r.ok = false; r.error = "timeout waiting for step_out to complete";
            return r;
        }
        r.ok = true;
        duint cip = DbgValFromString("cip");
        r.data = {{"action","step_out"},{"cip", formatHexU64(cip)}};
        return r;
    }
};

// ============= K-42 get_debug_state =============
// 只读，无副作用，不需 confirm。给 agent 一个"先查再动"的清晰入口，避免在错误状态
// 下盲调 run_continue / pause_debug / step_* 然后被 x64dbg 拒收。
// 即使 agent 忘了先查，K-42 的三个执行控制工具也都会在 error 里带回 current_state，
// 形成两层防线。
class GetDebugStateTool : public ITool {
public:
    std::string name() const override { return "get_debug_state"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "Query the current debugger state. Returns "
               "state=(not_debugging|running|paused) and, when paused, the current cip. "
               "ALWAYS call this before run_continue / pause_debug / step_in / step_over / "
               "step_out / run_until if you are not sure whether the debuggee is running "
               "or paused - those commands are rejected by x64dbg when the state is wrong.";
    }
    std::string descriptionZh() const override
    {
        return "查询调试器当前状态。返回 state=(not_debugging|running|paused)，paused 时附 cip。"
               "在不确定 debuggee 处于 running 还是 paused 时，调用 run_continue / pause_debug / "
               "step_in / step_over / step_out / run_until 之前 **务必先调本工具**——"
               "状态不对时这些命令会被 x64dbg 拒收。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
        };
    }
    ToolResult invoke(const nlohmann::json& /*args*/, ToolContext& ctx) override
    {
        ToolResult r;
        const char* st = currentDbgStateStr();
        r.ok = true;
        if (std::string(st) == "paused") {
            duint cip = DbgValFromString("cip");
            r.data = {{"state", st},
                      {"cip", formatHexU64(cip)},
                      {"debugging", true}};
        } else if (std::string(st) == "running") {
            r.data = {{"state", st}, {"debugging", true}};
        } else {
            r.data = {{"state", st}, {"debugging", false}};
        }
        (void)ctx;
        return r;
    }
};

void registerDebugNavigationTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<RunContinueTool>(), "execution-control");
    reg.registerTool(std::make_unique<PauseDebugTool>(), "execution-control");
    reg.registerTool(std::make_unique<StepOutTool>(), "execution-control");
    reg.registerTool(std::make_unique<GetDebugStateTool>(), "execution-control");
}

}  // namespace x64ai
