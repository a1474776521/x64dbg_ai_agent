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

#include <QtConcurrent/QtConcurrent>
#include <QThreadPool>
#include <QFuture>

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

// K-36: 粗估一条消息的 token 数（无 tokenizer，用"字符数/4"经验比例 + 角色/工具开销）。
std::size_t estimateMessageTokens(const ChatMessage& m)
{
    std::size_t chars = m.content.size() + m.reasoningContent.size();
    for (const auto& tc : m.toolCalls) {
        chars += tc.name.size() + tc.argumentsJson.size() + 8;
    }
    chars += m.toolName.size() + m.toolCallId.size();
    // 每条消息固定结构开销（role/分隔符等）≈ 4 tokens
    return chars / 4 + 4;
}

std::size_t estimateTokens(const std::vector<ChatMessage>& msgs)
{
    std::size_t t = 0;
    for (const auto& m : msgs) t += estimateMessageTokens(m);
    return t;
}

// K-36: 按模型名估算上下文窗口（token）。未知模型给保守默认。
std::size_t providerContextWindow(const std::string& modelRaw)
{
    std::string m = modelRaw;
    std::transform(m.begin(), m.end(), m.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto has = [&](const char* k) { return m.find(k) != std::string::npos; };

    // DeepSeek：官方 chat/reasoner 当前 64K 上下文
    if (has("deepseek")) return 64000;
    // OpenAI gpt-4o / 4.1 / o-series：128K
    if (has("gpt-4o") || has("gpt-4.1") || has("o1") || has("o3") || has("o4")) return 128000;
    if (has("gpt-4-turbo") || has("gpt-4-1106") || has("gpt-4-0125")) return 128000;
    if (has("gpt-3.5")) return 16000;
    if (has("gpt-4")) return 8000;  // 老 gpt-4 基础版
    // Anthropic via Copilot：Claude 3.x 200K
    if (has("claude")) return 200000;
    // 未知：保守 32K
    return 32000;
}

// K-36: 上下文压缩 —— 把最老的【整轮】(一条 assistant + 其后紧跟的全部 tool 消息)
// 折叠成一条 system 摘要消息。整轮折叠是为了保证 assistant.toolCalls 与后续
// tool.toolCallId 的配对不被拆散（OpenAI/DeepSeek 协议强约束，拆散会 400）。
//
// 约束：
//   - 开头连续的 system 消息（含 auto-RAG 注入）永不压缩
//   - 末尾 keepRounds 个"轮单元"永不压缩（保留近期上下文）
//   - 至少要能压掉一个轮单元才动手；压不动就原样返回 false
//
// 返回 true 表示发生了压缩。
bool compressOldestRound(std::vector<ChatMessage>& msgs, int keepRounds)
{
    if (msgs.size() < 4) return false;

    // 1) 定位"可压缩区"起点：跳过开头连续 system
    std::size_t head = 0;
    while (head < msgs.size() && msgs[head].role == "system") ++head;

    // 2) 把 [head, end) 切成"轮单元"：每个 user 或 assistant 起一轮，
    //    其后紧跟的 tool 消息归入同一轮。记录每轮的 [begin,end)。
    struct Round { std::size_t begin; std::size_t end; bool hasAssistantTools; };
    std::vector<Round> rounds;
    std::size_t i = head;
    while (i < msgs.size()) {
        Round r;
        r.begin = i;
        r.hasAssistantTools = (msgs[i].role == "assistant" && !msgs[i].toolCalls.empty());
        ++i;
        // 吸收紧跟的 tool 消息（属于上一条 assistant 的工具结果）
        while (i < msgs.size() && msgs[i].role == "tool") { ++i; }
        r.end = i;
        rounds.push_back(r);
    }

    // 3) 保留末尾 keepRounds 轮 + 至少要有可压的轮
    if (static_cast<int>(rounds.size()) <= keepRounds) return false;
    const std::size_t compressibleRounds = rounds.size() - static_cast<std::size_t>(keepRounds);
    if (compressibleRounds == 0) return false;

    // 4) 折叠最老的那一轮（rounds[0]）—— 只压一个轮单元，调用方循环直到达标
    const Round& target = rounds[0];

    std::ostringstream os;
    os << "[Compressed older turn (auto context-compression to fit model window). "
          "Original messages summarized below; re-run a tool if you need exact data.]\n";
    for (std::size_t k = target.begin; k < target.end; ++k) {
        const ChatMessage& m = msgs[k];
        os << "- " << m.role;
        if (!m.toolCalls.empty()) {
            os << " called:";
            for (const auto& tc : m.toolCalls) os << " " << tc.name << "()";
        }
        if (m.role == "tool") {
            os << " [" << m.toolName << "]";
        }
        // 内容摘要：每条最多留 300 字符
        std::string body = m.content;
        // 去掉换行噪声便于单行展示
        for (auto& ch : body) if (ch == '\n' || ch == '\r') ch = ' ';
        if (body.size() > 300) body = body.substr(0, 300) + "...";
        if (!body.empty()) os << ": " << body;
        os << "\n";
    }

    ChatMessage summary;
    summary.role    = "system";
    summary.content = os.str();

    // 5) 用 summary 替换 [target.begin, target.end)
    auto first = msgs.begin() + static_cast<std::ptrdiff_t>(target.begin);
    auto last  = msgs.begin() + static_cast<std::ptrdiff_t>(target.end);
    msgs.erase(first, last);
    msgs.insert(msgs.begin() + static_cast<std::ptrdiff_t>(target.begin), std::move(summary));
    return true;
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

        // K-36: 上下文压缩 —— 每轮 streamChat 前检查累计 token 是否超阈值，
        // 超则反复折叠最老整轮直到达标（或压不动）。整轮折叠保证 tool_call 配对不破。
        if (appCfg.contextCompressEnabled) {
            const std::string modelName =
                req.model.empty() ? req.provider->defaultModel() : req.model;
            const std::size_t window = providerContextWindow(modelName);
            const std::size_t budget =
                window * static_cast<std::size_t>(appCfg.contextCompressThresholdPct) / 100;
            std::size_t est = estimateTokens(req.messages);
            if (est > budget) {
                int folded = 0;
                while (est > budget &&
                       compressOldestRound(req.messages, appCfg.contextCompressKeepRounds)) {
                    ++folded;
                    est = estimateTokens(req.messages);
                }
                XAI_LOG_WARN("AgentLoop iter#{}: context-compress folded {} round(s); "
                             "est_tokens now={} budget={} (window={} pct={})",
                             iter, folded, est, budget, window,
                             appCfg.contextCompressThresholdPct);
            }
        }

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

        if (cancel.load()) {
            if (cb.onError) cb.onError("agent: cancelled during tool dispatch");
            return iter;
        }

        // dispatch + K-35 retry 封装；可在主线程串行调用，也可在线程池并发调用。
        // 仅做 ToolRegistry::dispatch + 退避重试，不碰 req.messages / cb（非线程安全部分留主线程）。
        auto runOneTool = [&](const ToolCall& tc) -> std::pair<ToolResult, long long> {
            const auto s0 = std::chrono::steady_clock::now();
            ToolResult tr = registry.dispatch(tc.name, tc.argumentsJson, ctx);

            // K-35: tool retry —— 仅对【非 Write】工具的【瞬时错误】退避重试。
            // K-37: 同时排除【DbgControl】类（wait_for_event / step_* / run_until）——
            //       其 "timeout" 几乎都是业务超时（没等到事件），重试只是再 timeout 一次，
            //       白白浪费 30-120 秒。实测脱壳场景 K-35 误命中 3 次浪费 90 秒。
            if (appCfg.toolRetryEnabled && appCfg.toolRetryMax > 0 &&
                isTransientToolError(tr) &&
                registry.categoryOf(tc.name) != ToolCategory::Write &&
                registry.categoryOf(tc.name) != ToolCategory::DbgControl) {
                for (int attempt = 1; attempt <= appCfg.toolRetryMax; ++attempt) {
                    if (cancel.load()) break;
                    const int backoffMs = 300 * attempt;
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
                    if (!isTransientToolError(tr)) break;
                }
            }
            const auto s1 = std::chrono::steady_clock::now();
            const long long ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(s1 - s0).count();
            return {std::move(tr), ms};
        };

        // K-36: 并行 read 判定 —— 仅当本批 tool_calls 全为 Read 类、开关开启、且数量>1
        // 时用线程池并发；只要有一个 DbgControl/Write 立即回退串行（保证 confirm/audit/
        // 写副作用时序）。
        const int callCount = static_cast<int>(st.toolCalls.size());
        bool allRead = true;
        for (const auto& tc : st.toolCalls) {
            if (registry.categoryOf(tc.name) != ToolCategory::Read) { allRead = false; break; }
        }
        const bool useParallel = appCfg.parallelReadEnabled && appCfg.parallelReadMax > 1 &&
                                 allRead && callCount > 1;

        // 结果按原始顺序收集（保证 tool 消息顺序 == tool_calls 顺序 → 配对正确）
        std::vector<std::pair<ToolResult, long long>> results(static_cast<std::size_t>(callCount));

        if (useParallel) {
            const int conc = std::min(appCfg.parallelReadMax, callCount);
            XAI_LOG_INFO("AgentLoop iter#{}: parallel-read {} tools (max_conc={})",
                         iter, callCount, conc);
            QThreadPool pool;
            pool.setMaxThreadCount(conc);
            std::vector<QFuture<void>> futures;
            futures.reserve(static_cast<std::size_t>(callCount));
            for (int idx = 0; idx < callCount; ++idx) {
                futures.push_back(QtConcurrent::run(&pool, [&, idx]() {
                    const ToolCall& tcl = st.toolCalls[static_cast<std::size_t>(idx)];
                    results[static_cast<std::size_t>(idx)] = runOneTool(tcl);
                }));
            }
            for (auto& f : futures) f.waitForFinished();
        } else {
            // 串行：与原行为一致；含 DbgControl/Write 走此路径
            for (int idx = 0; idx < callCount; ++idx) {
                if (cancel.load()) {
                    if (cb.onError) cb.onError("agent: cancelled during tool dispatch");
                    return iter;
                }
                results[static_cast<std::size_t>(idx)] =
                    runOneTool(st.toolCalls[static_cast<std::size_t>(idx)]);
            }
        }

        // 回调 + append tool 消息：始终在主循环线程、按原始顺序做（cb 非线程安全）
        for (int idx = 0; idx < callCount; ++idx) {
            const ToolCall&  tc  = st.toolCalls[static_cast<std::size_t>(idx)];
            ToolResult&      tr  = results[static_cast<std::size_t>(idx)].first;
            const long long  elapsedMs = results[static_cast<std::size_t>(idx)].second;

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
