// ai/copilot_chat_client.cpp
#include "ai/copilot_chat_client.h"

#include <atomic>
#include <map>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "ai/copilot_auth.h"
#include "ai/sse_parser.h"
#include "util/config.h"
#include "util/http_options.h"
#include "util/logging.h"

namespace x64ai {

namespace {

cpr::Header buildCommonHeaders(const std::string& chatToken)
{
    const auto& cfg = Config::instance().get();
    return cpr::Header{
        {"Authorization",          "Bearer " + chatToken},
        {"Accept",                 "application/json"},
        {"Content-Type",           "application/json"},
        {"Editor-Version",         cfg.copilot.editorVersion},
        {"Editor-Plugin-Version",  cfg.copilot.editorPluginVersion},
        {"Copilot-Integration-Id", cfg.copilot.copilotIntegrationId},
        {"OpenAI-Intent",          cfg.copilot.openaiIntent},
        {"User-Agent",             cfg.copilot.userAgent},
    };
}

// G-2 (2026-05-25): 把任意 OpenAI/Anthropic 风格的 usage 对象解析到 UsageInfo。
// 兼容字段：
//   OpenAI:    prompt_tokens / completion_tokens / total_tokens
//              prompt_tokens_details.cached_tokens
//              completion_tokens_details.reasoning_tokens
//   Anthropic: input_tokens / output_tokens
//              cache_read_input_tokens / cache_creation_input_tokens
UsageInfo parseUsage(const nlohmann::json& u)
{
    UsageInfo ui;
    // OpenAI 三件套
    ui.promptTokens     = u.value("prompt_tokens", 0);
    ui.completionTokens = u.value("completion_tokens", 0);
    ui.totalTokens      = u.value("total_tokens", 0);
    // Anthropic 风格映射到 OpenAI 命名
    if (ui.promptTokens == 0)     ui.promptTokens     = u.value("input_tokens", 0);
    if (ui.completionTokens == 0) ui.completionTokens = u.value("output_tokens", 0);
    if (ui.totalTokens == 0)      ui.totalTokens      = ui.promptTokens + ui.completionTokens;
    // OpenAI cached
    if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object()) {
        ui.cachedPromptTokens = u["prompt_tokens_details"].value("cached_tokens", 0);
    }
    // OpenAI reasoning
    if (u.contains("completion_tokens_details") && u["completion_tokens_details"].is_object()) {
        ui.reasoningTokens = u["completion_tokens_details"].value("reasoning_tokens", 0);
    }
    // Anthropic cache 字段
    if (ui.cachedPromptTokens == 0) ui.cachedPromptTokens = u.value("cache_read_input_tokens", 0);
    ui.cacheCreationTokens          = u.value("cache_creation_input_tokens", 0);
    return ui;
}

}  // namespace

CopilotChatClient& CopilotChatClient::instance()
{
    static CopilotChatClient inst;
    return inst;
}

bool CopilotChatClient::isAuthenticated(std::string* outReason) const
{
    auto user = CopilotAuth::instance().currentUserLogin();
    if (user && !user->empty()) return true;
    if (outReason) *outReason = "未登录 GitHub Copilot";
    return false;
}

std::string CopilotChatClient::defaultModel() const
{
    return Config::instance().get().defaultModel;
}

void CopilotChatClient::streamChat(const ChatRequest& req, const ChatStreamCallbacks& cb)
{
    const auto& cfg = Config::instance().get();

    auto tok = CopilotAuth::instance().getChatToken();
    if (!tok) {
        if (cb.onError) cb.onError("无法获取 Copilot chat token（请确认已 gh/VSCode 登录）");
        return;
    }

    nlohmann::json body = {
        {"model",       req.model.empty() ? cfg.defaultModel : req.model},
        {"stream",      req.stream},
        {"temperature", req.temperature},
    };
    if (req.maxTokens > 0) body["max_tokens"] = req.maxTokens;
    // G-2 (2026-05-25): 要求 SSE 末尾发 usage chunk；Copilot 透传 OpenAI/Anthropic 字段
    if (req.stream) {
        body["stream_options"] = {{"include_usage", true}};
    }

    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& m : req.messages) {
        nlohmann::json jm = {{"role", m.role}, {"content", m.content}};
        // K-40: assistant 携带 tool_calls（OpenAI 协议；Copilot proxy 透传到上游 Claude/GPT）
        if (m.role == "assistant" && !m.toolCalls.empty()) {
            nlohmann::json tcs = nlohmann::json::array();
            for (const auto& tc : m.toolCalls) {
                tcs.push_back({
                    {"id",   tc.id},
                    {"type", "function"},
                    {"function", {
                        {"name",      tc.name},
                        {"arguments", tc.argumentsJson},
                    }},
                });
            }
            jm["tool_calls"] = std::move(tcs);
            if (m.content.empty()) jm["content"] = nullptr;
        }
        // K-40: role=tool 必须带 tool_call_id
        if (m.role == "tool") {
            jm["tool_call_id"] = m.toolCallId;
            if (!m.toolName.empty()) jm["name"] = m.toolName;
        }
        msgs.push_back(std::move(jm));
    }
    body["messages"] = std::move(msgs);

    // === K-40: tools / tool_choice 透传 ===
    // Copilot proxy 把 OpenAI 风格的 tools 转发给上游模型（GPT-4o/Claude/Opus 4.x）
    // Claude 4 系列原生 tool_use，proxy 再转成 OpenAI tool_calls 格式回吐。
    if (!req.tools.empty()) {
        nlohmann::json tools = nlohmann::json::array();
        for (const auto& t : req.tools) {
            nlohmann::json params;
            try {
                params = t.parametersJson.empty() ? nlohmann::json::object()
                                                  : nlohmann::json::parse(t.parametersJson);
            } catch (...) {
                params = nlohmann::json::object();
            }
            tools.push_back({
                {"type", "function"},
                {"function", {
                    {"name",        t.name},
                    {"description", t.description},
                    {"parameters",  std::move(params)},
                }},
            });
        }
        body["tools"] = std::move(tools);
        if (!req.toolChoice.empty()) body["tool_choice"] = req.toolChoice;
    }

    const std::string url     = cfg.copilot.apiBase + "/chat/completions";
    const std::string payload = body.dump();

    XAI_LOG_INFO("POST {} model={} stream={} msgs={}",
                 url, body["model"].get<std::string>(), req.stream, req.messages.size());

    auto headers = buildCommonHeaders(tok->token);

    if (!req.stream) {
        cpr::Response r = cpr::Post(
            cpr::Url{url}, headers, cpr::Body{payload},
            defaultSslOptions(),
            cpr::Timeout{cfg.httpTimeoutMs});
        if (r.error) {
            if (cb.onError) cb.onError("HTTP 错误: " + r.error.message);
            return;
        }
        if (r.status_code < 200 || r.status_code >= 300) {
            if (cb.onError) {
                cb.onError("HTTP " + std::to_string(r.status_code) + ": " + r.text);
            }
            return;
        }
        try {
            auto j = nlohmann::json::parse(r.text);
            if (j.contains("choices") && !j["choices"].empty()) {
                const auto& choice = j["choices"][0];
                const auto& msg = choice["message"];
                if (msg.contains("content") && msg["content"].is_string() && cb.onDelta) {
                    cb.onDelta(msg["content"].get<std::string>());
                }
                // K-40: 非流式 tool_calls 解析
                if (msg.contains("tool_calls") && msg["tool_calls"].is_array() && cb.onToolCalls) {
                    std::vector<ToolCall> calls;
                    for (const auto& tc : msg["tool_calls"]) {
                        ToolCall c;
                        if (tc.contains("id") && tc["id"].is_string())
                            c.id = tc["id"].get<std::string>();
                        if (tc.contains("function") && tc["function"].is_object()) {
                            const auto& f = tc["function"];
                            if (f.contains("name") && f["name"].is_string())
                                c.name = f["name"].get<std::string>();
                            if (f.contains("arguments") && f["arguments"].is_string())
                                c.argumentsJson = f["arguments"].get<std::string>();
                        }
                        if (!c.name.empty()) calls.push_back(std::move(c));
                    }
                    if (!calls.empty()) cb.onToolCalls(std::move(calls));
                }
            }
            // G-2: 非流式 usage
            if (j.contains("usage") && j["usage"].is_object() && cb.onUsage) {
                cb.onUsage(parseUsage(j["usage"]));
            }
            if (cb.onDone) cb.onDone();
        } catch (const std::exception& e) {
            if (cb.onError) cb.onError(std::string("解析响应失败: ") + e.what());
        }
        return;
    }

    // 流式：用 SseParser + cpr WriteCallback
    std::atomic_bool finished{false};
    std::string      errorBuf;

    // K-40: tool_calls 流式增量拼装（按 index 区分多个 call，每个 id/name/arguments 分片到达）
    struct PartialToolCall {
        std::string id;
        std::string name;
        std::string argumentsJson;
    };
    std::map<int, PartialToolCall> partialCalls;
    bool sawToolCalls = false;

    SseParser parser([&](std::string_view data) {
        if (data == "[DONE]") {
            finished = true;
            // K-40: 先回吐 tool_calls，再 onDone
            if (sawToolCalls && cb.onToolCalls) {
                std::vector<ToolCall> calls;
                calls.reserve(partialCalls.size());
                for (auto& [idx, p] : partialCalls) {
                    ToolCall c;
                    c.id = std::move(p.id);
                    c.name = std::move(p.name);
                    c.argumentsJson = std::move(p.argumentsJson);
                    if (!c.name.empty()) calls.push_back(std::move(c));
                }
                if (!calls.empty()) cb.onToolCalls(std::move(calls));
            }
            if (cb.onDone) cb.onDone();
            return;
        }
        try {
            auto j = nlohmann::json::parse(data);
            // G-2: SSE usage chunk
            if (j.contains("usage") && j["usage"].is_object() && cb.onUsage) {
                cb.onUsage(parseUsage(j["usage"]));
            }
            if (!j.contains("choices") || j["choices"].empty()) return;
            const auto& choice = j["choices"][0];
            if (choice.contains("delta") && choice["delta"].is_object()) {
                const auto& d = choice["delta"];
                if (d.contains("content") && d["content"].is_string()) {
                    if (cb.onDelta) cb.onDelta(d["content"].get<std::string>());
                }
                // K-40: tool_calls 流式增量
                if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
                    sawToolCalls = true;
                    for (const auto& tc : d["tool_calls"]) {
                        int idx = tc.value("index", 0);
                        auto& p = partialCalls[idx];
                        if (tc.contains("id") && tc["id"].is_string()) {
                            p.id = tc["id"].get<std::string>();
                        }
                        if (tc.contains("function") && tc["function"].is_object()) {
                            const auto& f = tc["function"];
                            if (f.contains("name") && f["name"].is_string()) {
                                if (p.name.empty()) p.name = f["name"].get<std::string>();
                                else                p.name += f["name"].get<std::string>();
                            }
                            if (f.contains("arguments") && f["arguments"].is_string()) {
                                p.argumentsJson += f["arguments"].get<std::string>();
                            }
                        }
                    }
                }
            } else if (choice.contains("message") && choice["message"].is_object()) {
                // 兼容某些 Copilot 上游一次性整条 message 返回（非增量）
                const auto& m = choice["message"];
                if (m.contains("content") && m["content"].is_string()) {
                    if (cb.onDelta) cb.onDelta(m["content"].get<std::string>());
                }
                if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                    sawToolCalls = true;
                    int idx = 0;
                    for (const auto& tc : m["tool_calls"]) {
                        auto& p = partialCalls[idx++];
                        if (tc.contains("id") && tc["id"].is_string())
                            p.id = tc["id"].get<std::string>();
                        if (tc.contains("function") && tc["function"].is_object()) {
                            const auto& f = tc["function"];
                            if (f.contains("name") && f["name"].is_string())
                                p.name = f["name"].get<std::string>();
                            if (f.contains("arguments") && f["arguments"].is_string())
                                p.argumentsJson = f["arguments"].get<std::string>();
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            XAI_LOG_WARN("SSE chunk parse error: {}; data={}", e.what(), std::string(data));
        }
    });

    // cpr 1.14 的 WriteCallback 形参为 (std::string_view data, intptr_t userdata)
    cpr::WriteCallback writer{[&](std::string_view data, intptr_t /*userdata*/) -> bool {
        parser.feed(data);
        return true;
    }, 0};

    cpr::Response r = cpr::Post(
        cpr::Url{url}, headers, cpr::Body{payload},
        defaultSslOptions(),
        streamTimeout(), streamLowSpeed(), writer);

    if (r.error) {
        if (cb.onError) cb.onError("HTTP 错误: " + r.error.message);
        return;
    }
    if (r.status_code < 200 || r.status_code >= 300) {
        if (cb.onError) {
            cb.onError("HTTP " + std::to_string(r.status_code) + ": " + r.text);
        }
        return;
    }

    if (!finished && cb.onDone) {
        // K-40: 连接断早了没收到 [DONE]，也把已拼装好的 tool_calls 回吐
        if (sawToolCalls && cb.onToolCalls) {
            std::vector<ToolCall> calls;
            calls.reserve(partialCalls.size());
            for (auto& [idx, p] : partialCalls) {
                ToolCall c;
                c.id = std::move(p.id);
                c.name = std::move(p.name);
                c.argumentsJson = std::move(p.argumentsJson);
                if (!c.name.empty()) calls.push_back(std::move(c));
            }
            if (!calls.empty()) cb.onToolCalls(std::move(calls));
        }
        cb.onDone();
    }
}

std::vector<std::string> CopilotChatClient::listModels()
{
    const auto& cfg = Config::instance().get();

    auto tok = CopilotAuth::instance().getChatToken();
    if (!tok) {
        XAI_LOG_WARN("listModels: no chat token");
        return {};
    }

    auto headers = buildCommonHeaders(tok->token);
    const std::string url = cfg.copilot.apiBase + "/models";

    cpr::Response r = cpr::Get(
        cpr::Url{url}, headers,
        defaultSslOptions(),
        cpr::Timeout{cfg.httpTimeoutMs});

    if (r.error || r.status_code < 200 || r.status_code >= 300) {
        XAI_LOG_ERROR("listModels http {}: {}", r.status_code,
                      r.error ? r.error.message : r.text);
        return {};
    }

    std::vector<std::string> ids;
    try {
        auto j = nlohmann::json::parse(r.text);
        // 兼容 OpenAI 风格 {"data":[{"id": "..."}, ...]}
        if (j.contains("data") && j["data"].is_array()) {
            for (const auto& m : j["data"]) {
                if (m.contains("id") && m["id"].is_string()) {
                    ids.push_back(m["id"].get<std::string>());
                }
            }
        }
    } catch (const std::exception& e) {
        XAI_LOG_ERROR("listModels parse error: {}", e.what());
    }
    XAI_LOG_INFO("listModels -> {} entries", ids.size());
    return ids;
}

}  // namespace x64ai
