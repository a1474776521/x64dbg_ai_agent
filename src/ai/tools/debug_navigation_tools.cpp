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
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"wait_for_stop", {{"type", "boolean"},
                                   {"description", "Block until next stop; default false"}}},
                {"timeout_ms",    {{"type", "integer"},
                                   {"description", "Wait timeout when wait_for_stop=true; default 30000, max 60000"}}},
            }},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        bool wait = false;
        if (args.contains("wait_for_stop") && args["wait_for_stop"].is_boolean()) {
            wait = args["wait_for_stop"].get<bool>();
        }
        int timeoutMs = 30000;
        std::string e;
        if (tryGetInt32Hint(args, "timeout_ms", 100, 60000, timeoutMs, e)) {}
        else if (!e.empty()) { r.ok=false; r.error="invalid 'timeout_ms': "+e; return r; }

        if (!DbgCmdExecDirect("run")) {
            r.ok = false; r.error = "run command failed"; return r;
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
        int timeoutMs = 5000;
        std::string e;
        if (tryGetInt32Hint(args, "timeout_ms", 100, 30000, timeoutMs, e)) {}
        else if (!e.empty()) { r.ok=false; r.error="invalid 'timeout_ms': "+e; return r; }

        if (!DbgCmdExecDirect("pause")) {
            r.ok = false; r.error = "pause command failed"; return r;
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
        int timeoutMs = 30000;
        std::string e;
        if (tryGetInt32Hint(args, "timeout_ms", 100, 60000, timeoutMs, e)) {}
        else if (!e.empty()) { r.ok=false; r.error="invalid 'timeout_ms': "+e; return r; }

        if (!DbgCmdExecDirect("StepOut")) {
            r.ok = false; r.error = "StepOut command failed"; return r;
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

void registerDebugNavigationTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<RunContinueTool>());
    reg.registerTool(std::make_unique<PauseDebugTool>());
    reg.registerTool(std::make_unique<StepOutTool>());
}

}  // namespace x64ai
