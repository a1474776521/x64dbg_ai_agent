// ai/agent_loop.cpp

#include "ai/agent_loop.h"

#include "ai/tools/tool.h"
#include "ai/tools/tool_registry.h"

#include "ai/embedding_client.h"
#include "storage/session_store.h"

#include "util/config.h"
#include "util/logging.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <exception>
#include <sstream>
#include <thread>

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

// K-35: 判断一次失败的 tool 结果是否属于"瞬时/可恢复"错误，值得重试。
// 仅对网络/超时/HTTP 5xx/connection/embedding 这类外部依赖抖动重试；
// 参数错误、校验失败、业务逻辑失败（如 "no active session"）不重试。
bool isTransientToolError(const ToolResult& tr)
{
    if (tr.ok) return false;
    std::string e = tr.error;
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (e.empty()) return false;

    // 可重试白名单关键字
    static const char* kTransient[] = {
        "timeout", "timed out", "connection", "connect failed",
        "network", "temporarily", "temporary failure", "embedding failed",
        "rate limit", "too many requests", "503", "502", "504", "500",
        "service unavailable", "reset by peer", "broken pipe", "ssl",
    };
    for (const char* k : kTransient) {
        if (e.find(k) != std::string::npos) return true;
    }
    return false;
}

// K-35: auto-RAG 注入 —— 取首条 user 消息做向量检索，
// 把 top-K 历史分析 chunks 拼成一条 system 上下文消息。
// 失败（无 store / embed 失败 / 无结果）返回空串，调用方静默跳过。
std::string buildAutoRagContext(const std::vector<ChatMessage>& messages,
                                ToolContext&                     ctx,
                                int                              topK)
{
    if (!ctx.sessionStore) return {};

    // 取第一条 user 消息文本作为查询
    std::string query;
    for (const auto& m : messages) {
        if (m.role == "user" && !m.content.empty()) {
            query = m.content;
            break;
        }
    }
    if (query.empty()) return {};
    // 过长的 query 截断，省 embedding 配额（首条 user 一般是问句，不会太长）
    if (query.size() > 4000) query = query.substr(0, 4000);

    auto emb = EmbeddingClient::instance().embed(query);
    if (emb.empty()) {
        XAI_LOG_WARN("AgentLoop auto-RAG: embedding failed, skip injection");
        return {};
    }

    auto* store = reinterpret_cast<SessionStore*>(ctx.sessionStore);
    auto results = store->searchSimilar(emb, topK);
    if (results.empty()) return {};

    std::ostringstream os;
    os << "[Auto-injected context from prior analysis of this debuggee "
          "(vector search, most relevant first). Use as background; verify "
          "against live tools before relying on it.]\n";
    int idx = 0;
    for (const auto& rc : results) {
        ++idx;
        os << "\n#" << idx << " (kind=" << rc.chunk.kind;
        if (rc.chunk.va) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " va=0x%llx",
                          static_cast<unsigned long long>(rc.chunk.va));
            os << buf;
        }
        os << ", distance=" << rc.distance << ")\n";
        // 单条 chunk 文本上限，避免注入炸 context
        std::string text = rc.chunk.text;
        if (text.size() > 1200) text = text.substr(0, 1200) + "...(truncated)";
        os << text << "\n";
    }
    return os.str();
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

    const auto& appCfg = Config::instance().get();

    // K-35: auto-RAG 注入 —— 在首轮 LLM 调用前，把历史分析检索结果作为 system 上下文插入。
    // 仅注入一次（run 入口）；后续轮次靠 LLM 显式 rag_search 深挖。
    if (appCfg.autoRagInjectEnabled) {
        std::string ragCtx = buildAutoRagContext(req.messages, ctx, appCfg.autoRagTopK);
        if (!ragCtx.empty()) {
            ChatMessage rm;
            rm.role    = "system";
            rm.content = std::move(ragCtx);
            // 插到最后一条 user 消息之前，让模型把它当"既有背景"读
            auto insertPos = req.messages.end();
            for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
                if (it->role == "user") { insertPos = it.base() - 1; break; }
            }
            req.messages.insert(insertPos, std::move(rm));
            XAI_LOG_INFO("AgentLoop: auto-RAG context injected (top_k={})", appCfg.autoRagTopK);
        }
    }

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

            // K-35: tool retry —— 仅对【非 Write】工具的【瞬时错误】退避重试。
            // Write 类（断点/dbg cmd/patch）已确认的副作用不能重复触发，永不重试。
            if (appCfg.toolRetryEnabled && appCfg.toolRetryMax > 0 &&
                isTransientToolError(tr) &&
                registry.categoryOf(tc.name) != ToolCategory::Write) {
                for (int attempt = 1; attempt <= appCfg.toolRetryMax; ++attempt) {
                    if (cancel.load()) break;
                    const int backoffMs = 300 * attempt;  // 300ms / 600ms / 900ms
                    XAI_LOG_WARN("AgentLoop iter#{} tool='{}' transient error '{}' "
                                 "-> retry {}/{} after {}ms",
                                 iter, tc.name, tr.error, attempt, appCfg.toolRetryMax, backoffMs);
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                    if (cancel.load()) break;
                    tr = registry.dispatch(tc.name, tc.argumentsJson, ctx);
                    if (tr.ok) {
                        XAI_LOG_INFO("AgentLoop iter#{} tool='{}' recovered on retry {}/{}",
                                     iter, tc.name, attempt, appCfg.toolRetryMax);
                        break;
                    }
                    if (!isTransientToolError(tr)) break;  // 变成非瞬时错误，停止重试
                }
            }
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
