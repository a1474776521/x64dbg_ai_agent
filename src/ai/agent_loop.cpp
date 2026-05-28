// ai/agent_loop.cpp

#include "ai/agent_loop.h"

#include "ai/tools/tool.h"
#include "ai/tools/tool_registry.h"

#include "util/logging.h"

#include <chrono>
#include <exception>
#include <sstream>

namespace x64ai {

namespace {

bool providerSupportsTools(IChatProvider* p)
{
    if (!p) return false;
    // Copilot 上游禁用第三方 tools；DeepSeek 原生支持
    return p->kind() == ProviderKind::DeepSeek;
}

std::string argsDigest(const std::string& argsJson, std::size_t maxLen = 200)
{
    if (argsJson.size() <= maxLen) return argsJson;
    return argsJson.substr(0, maxLen) + "...(truncated)";
}

}  // namespace

int AgentLoop::run(AgentRunRequest&         req,
                   ToolContext&             ctx,
                   const AgentRunCallbacks& cb,
                   const std::atomic<bool>& cancel)
{
    if (!req.provider) {
        if (cb.onError) cb.onError("agent: provider is null");
        return 0;
    }

    auto& registry = ToolRegistry::instance();
    const bool useTools = providerSupportsTools(req.provider);

    std::vector<ChatTool> tools;
    if (useTools) {
        // 优先用调用方按预设过滤的白名单；否则取全量
        tools = !req.tools.empty() ? req.tools : registry.listChatTools();
        XAI_LOG_INFO("AgentLoop: provider={} tools={} (preset_filtered={}) maxIter={}",
                     req.provider->displayName(), tools.size(),
                     !req.tools.empty(), req.maxIter);
    } else {
        XAI_LOG_INFO("AgentLoop: provider={} (tools disabled — fallback to plain chat)",
                     req.provider->displayName());
    }

    int iter = 0;
    while (iter < req.maxIter) {
        if (cancel.load()) {
            if (cb.onError) cb.onError("agent: cancelled");
            return iter;
        }
        ++iter;

        // 本轮捕获用的临时状态
        struct RoundState {
            std::string             content;
            std::string             reasoning;
            std::vector<ToolCall>   toolCalls;
            std::string             error;
            bool                    done = false;
        } st;

        ChatRequest creq;
        creq.model        = req.model.empty() ? req.provider->defaultModel() : req.model;
        creq.messages     = req.messages;
        creq.stream       = true;
        creq.temperature  = req.temperature;
        creq.maxTokens    = req.maxTokens;
        if (useTools && !tools.empty()) {
            creq.tools      = tools;
            creq.toolChoice = "auto";
        }

        ChatStreamCallbacks scb;
        scb.onDelta = [&](std::string_view d) {
            st.content.append(d);
            if (cb.onAssistantDelta) cb.onAssistantDelta(d);
        };
        scb.onReasoningDelta = [&](std::string_view d) {
            st.reasoning.append(d);
            if (cb.onAssistantReasoningDelta) cb.onAssistantReasoningDelta(d);
        };
        scb.onError = [&](std::string e) {
            st.error = std::move(e);
        };
        scb.onDone = [&]() {
            st.done = true;
        };
        scb.onToolCalls = [&](std::vector<ToolCall> calls) {
            st.toolCalls = std::move(calls);
        };
        // G-2: usage 透传给 worker，由 UI 展示 cache hit ratio
        scb.onUsage = [&](const UsageInfo& u) {
            if (cb.onUsage) cb.onUsage(u);
        };

        try {
            req.provider->streamChat(creq, scb);
        } catch (const std::exception& e) {
            st.error = std::string("provider threw: ") + e.what();
        } catch (...) {
            st.error = "provider threw unknown exception";
        }

        if (!st.error.empty()) {
            XAI_LOG_ERROR("AgentLoop iter#{}: provider error: {}", iter, st.error);
            if (cb.onError) cb.onError(st.error);
            return iter;
        }

        // 追加 assistant 消息到对话历史
        ChatMessage am;
        am.role             = "assistant";
        am.content          = st.content;
        am.toolCalls        = st.toolCalls;
        am.reasoningContent = st.reasoning;
        req.messages.push_back(am);
        if (cb.onAssistantMessage) cb.onAssistantMessage(am);

        // 没 tool_calls → 终止
        if (st.toolCalls.empty()) {
            XAI_LOG_INFO("AgentLoop: done at iter#{} (no tool_calls)", iter);
            if (cb.onDone) cb.onDone();
            return iter;
        }

        // 顺序执行 tool_calls
        for (const auto& tc : st.toolCalls) {
            if (cancel.load()) {
                if (cb.onError) cb.onError("agent: cancelled during tool dispatch");
                return iter;
            }

            const auto t0 = std::chrono::steady_clock::now();
            ToolResult tr = registry.dispatch(tc.name, tc.argumentsJson, ctx);
            const auto t1 = std::chrono::steady_clock::now();
            const long long elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

            const std::string serialized = ToolRegistry::serializeForLlm(tr);

            AgentToolCallReport rep;
            rep.id            = tc.id;
            rep.name          = tc.name;
            rep.argumentsJson = tc.argumentsJson;
            rep.resultJson    = serialized;
            rep.ok            = tr.ok;
            rep.error         = tr.error;
            rep.elapsedMs     = elapsedMs;
            rep.truncated     = tr.truncatedTo > 0;
            if (cb.onToolReport) cb.onToolReport(rep);

            XAI_LOG_INFO("AgentLoop iter#{} tool='{}' ok={} elapsed={}ms truncated={} args={}",
                         iter, tc.name, tr.ok, elapsedMs, rep.truncated,
                         argsDigest(tc.argumentsJson));
            if (!tr.ok) {
                XAI_LOG_WARN("AgentLoop iter#{} tool='{}' error: {}",
                             iter, tc.name, tr.error.empty() ? "(empty)" : tr.error);
            }

            ChatMessage tm;
            tm.role       = "tool";
            tm.content    = serialized;
            tm.toolCallId = tc.id;
            tm.toolName   = tc.name;
            req.messages.push_back(tm);
        }

        // 继续下一轮
    }

    // 达到 maxIter；最后一轮如果产生了 tool_calls，对应 tool 消息已经追加，下一次 run 可以续上
    int pendingCalls = 0;
    if (!req.messages.empty() && req.messages.back().role == "tool") {
        // 反查最近的 assistant 消息
        for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
            if (it->role == "assistant") {
                pendingCalls = static_cast<int>(it->toolCalls.size());
                break;
            }
        }
    }
    XAI_LOG_WARN("AgentLoop: maxIter={} reached, pendingCalls={}", req.maxIter, pendingCalls);
    if (cb.onMaxIterReached) cb.onMaxIterReached(iter, pendingCalls);
    return iter;
}

}  // namespace x64ai
