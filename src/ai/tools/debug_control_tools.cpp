// ai/tools/debug_control_tools.cpp
//
// S2-D：调试控制 / 等待类工具。当前仅一个：
//
//   wait_for_event   阻塞等待调试器事件（breakpoint/paused/stepped），timeout 上限 60s。
//
// 设计要点：
//   - 必须在 AgentWorker 后台线程跑（AgentLoop 本身就在 QtConcurrent::run 里），
//     绝不能阻塞 GUI 线程。
//   - 用 EventBus::waitOnce 等，内部 50ms 切片 + 周期性查 ctx.cancelFlag，
//     用户在 UI 点"取消"能秒级响应；调试器调 cancelAllWaits 也会立即返回 false。
//   - 不直接 expose 给 system_prompt 强制使用；agent 自行选择。
//
// 输入：
//   {
//     "event":   "breakpoint" | "paused" | "stepped" | "any",   // 必填
//     "timeout_ms": 5000                                         // 可选；默认 5000，[100, 60000]
//   }
//
// 输出：
//   ok=true   {"event":"breakpoint", "addr":"0x401234", "seq":42}
//   ok=false  {"reason":"timeout" | "cancelled" | "not_debugging" | ...}
//
// "any" 实现：同时等 Breakpoint / Paused / Stepped；任一先到先返回。
// 简单做法：循环 50ms 轮询三种类型 waitOnce(50ms)，直到 timeout 或 cancel。
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

const char* eventToStr(DbgEvent e)
{
    switch (e) {
        case DbgEvent::Breakpoint:   return "breakpoint";
        case DbgEvent::Paused:       return "paused";
        case DbgEvent::Resumed:      return "resumed";
        case DbgEvent::Stepped:      return "stepped";
        case DbgEvent::DebugStarted: return "debug_started";
        case DbgEvent::DebugStopped: return "debug_stopped";
        default:                     return "unknown";
    }
}

}  // namespace

class WaitForEventTool : public ITool {
public:
    std::string name() const override { return "wait_for_event"; }
    std::string description() const override
    {
        return "Block until the debugger emits a specified event "
               "(breakpoint / paused / stepped) or the timeout expires. "
               "Use this AFTER you have started/resumed the debuggee with a "
               "command and need to wait for it to stop again. "
               "Always set a sensible timeout_ms; do NOT use this to poll.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"event", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({
                        "breakpoint", "paused", "stepped", "any"
                    })},
                    {"description", "Which debugger event to wait for."},
                }},
                {"timeout_ms", {
                    {"type", "integer"},
                    {"description", "Timeout in milliseconds. Default 5000, range [100, 60000]."},
                }},
            }},
            {"required", nlohmann::json::array({"event"})},
        };
    }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active";
            return r;
        }

        if (!args.contains("event") || !args["event"].is_string()) {
            r.ok = false; r.error = "'event' is required (string)";
            return r;
        }
        const std::string ev = args["event"].get<std::string>();

        int timeoutMs = 5000;
        {
            std::string err;
            if (tryGetInt32Hint(args, "timeout_ms", 100, 60000, timeoutMs, err)) {
                // ok
            } else if (!err.empty()) {
                r.ok = false; r.error = "invalid 'timeout_ms': " + err; return r;
            }
        }

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

        auto& bus = EventBus::instance();
        DbgEventPayload payload{};
        DbgEvent gotKind = DbgEvent::Paused;
        bool got = false;

        if (ev == "breakpoint" || ev == "paused" || ev == "stepped") {
            DbgEvent target;
            if      (ev == "breakpoint") target = DbgEvent::Breakpoint;
            else if (ev == "paused")     target = DbgEvent::Paused;
            else                          target = DbgEvent::Stepped;
            got = bus.waitOnce(target, std::chrono::milliseconds(timeoutMs),
                               &payload, ctx.cancelFlag);
            gotKind = target;
        } else if (ev == "any") {
            // 三路 50ms 轮询；任一先到先返回。
            const auto slice = std::chrono::milliseconds(50);
            while (std::chrono::steady_clock::now() < deadline) {
                if (ctx.cancelFlag && ctx.cancelFlag->load()) break;
                if (bus.waitOnce(DbgEvent::Breakpoint, slice, &payload, ctx.cancelFlag)) {
                    got = true; gotKind = DbgEvent::Breakpoint; break;
                }
                if (bus.waitOnce(DbgEvent::Paused, slice, &payload, ctx.cancelFlag)) {
                    got = true; gotKind = DbgEvent::Paused; break;
                }
                if (bus.waitOnce(DbgEvent::Stepped, slice, &payload, ctx.cancelFlag)) {
                    got = true; gotKind = DbgEvent::Stepped; break;
                }
            }
        } else {
            r.ok = false;
            r.error = "unknown 'event': " + ev +
                      " (expected breakpoint | paused | stepped | any)";
            return r;
        }

        if (!got) {
            // 区分超时 vs 用户取消 vs 调试停止
            std::string reason = "timeout";
            if (ctx.cancelFlag && ctx.cancelFlag->load()) reason = "cancelled";
            else if (!DbgIsDebugging())                   reason = "debug_stopped";
            r.ok = false;
            r.error = reason;
            r.data  = {{"reason", reason}, {"event", ev}, {"timeout_ms", timeoutMs}};
            XAI_LOG_INFO("wait_for_event: {} ({}) timeout/cancel after {}ms",
                         ev, reason, timeoutMs);
            return r;
        }

        r.ok = true;
        r.data = {
            {"event",  eventToStr(gotKind)},
            {"addr",   formatHexU64(payload.addr)},
            {"seq",    payload.seq},
        };
        XAI_LOG_INFO("wait_for_event: {} fired addr={} seq={}",
                     eventToStr(gotKind), formatHexU64(payload.addr), payload.seq);
        return r;
    }
};

void registerDebugControlTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<WaitForEventTool>());
}

}  // namespace x64ai
