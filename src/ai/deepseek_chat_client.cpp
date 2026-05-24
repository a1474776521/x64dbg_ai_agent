// ai/deepseek_chat_client.cpp
#include "ai/deepseek_chat_client.h"

#include <atomic>
#include <map>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "ai/sse_parser.h"
#include "util/config.h"
#include "util/http_options.h"
#include "util/logging.h"
#include "util/secret_store.h"

namespace x64ai {

namespace {

constexpr const char* kSecretName = "deepseek_api_key";

cpr::Header buildHeaders(const std::string& apiKey)
{
    return cpr::Header{
        {"Authorization", "Bearer " + apiKey},
        {"Accept",        "application/json"},
        {"Content-Type",  "application/json"},
        {"User-Agent",    "x64dbg-ai-plugin/1.0"},
    };
}

}  // namespace

DeepSeekChatClient& DeepSeekChatClient::instance()
{
    static DeepSeekChatClient inst;
    return inst;
}

// ====== Key 管理 ======

bool DeepSeekChatClient::saveApiKey(const std::string& key)
{
    if (key.empty()) return false;
    bool ok = SecretStore::instance().saveSecret(kSecretName, key);
    XAI_LOG_INFO("DeepSeek: saveApiKey ok={}", ok);
    return ok;
}

bool DeepSeekChatClient::clearApiKey()
{
    bool ok = SecretStore::instance().deleteSecret(kSecretName);
    XAI_LOG_INFO("DeepSeek: clearApiKey ok={}", ok);
    return ok;
}

std::optional<std::string> DeepSeekChatClient::loadApiKey() const
{
    return SecretStore::instance().loadSecret(kSecretName);
}

std::string DeepSeekChatClient::maskedApiKey() const
{
    auto k = loadApiKey();
    if (!k || k->empty()) return {};
    const std::string& s = *k;
    if (s.size() <= 12) return std::string(s.size(), '*');
    return s.substr(0, 8) + "****" + s.substr(s.size() - 4);
}

bool DeepSeekChatClient::isAuthenticated(std::string* outReason) const
{
    auto k = loadApiKey();
    if (!k || k->empty()) {
        if (outReason) *outReason = "未设置 DeepSeek API Key";
        return false;
    }
    return true;
}

// ====== /models ======

std::vector<std::string> DeepSeekChatClient::listModels()
{
    auto keyOpt = loadApiKey();
    if (!keyOpt || keyOpt->empty()) {
        XAI_LOG_WARN("DeepSeek listModels: no api key");
        return {};
    }

    const auto& cfg = Config::instance().get();
    const std::string url = cfg.deepseek.apiBase + "/models";

    cpr::Response r = cpr::Get(
        cpr::Url{url}, buildHeaders(*keyOpt),
        defaultSslOptions(),
        cpr::Timeout{cfg.httpTimeoutMs});

    if (r.error || r.status_code < 200 || r.status_code >= 300) {
        XAI_LOG_ERROR("DeepSeek listModels http {}: {}", r.status_code,
                      r.error ? r.error.message : r.text);
        return {};
    }

    std::vector<std::string> ids;
    try {
        auto j = nlohmann::json::parse(r.text);
        if (j.contains("data") && j["data"].is_array()) {
            for (const auto& m : j["data"]) {
                if (m.contains("id") && m["id"].is_string()) {
                    ids.push_back(m["id"].get<std::string>());
                }
            }
        }
    } catch (const std::exception& e) {
        XAI_LOG_ERROR("DeepSeek listModels parse error: {}", e.what());
    }
    XAI_LOG_INFO("DeepSeek listModels -> {} entries", ids.size());
    return ids;
}

// ====== /chat/completions ======

void DeepSeekChatClient::streamChat(const ChatRequest& req, const ChatStreamCallbacks& cb)
{
    auto keyOpt = loadApiKey();
    if (!keyOpt || keyOpt->empty()) {
        if (cb.onError) cb.onError("未设置 DeepSeek API Key（请点登录设置）");
        return;
    }
    const std::string apiKey = *keyOpt;

    const auto& cfg = Config::instance().get();
    const std::string url = cfg.deepseek.apiBase + "/chat/completions";

    nlohmann::json body = {
        {"model",       req.model.empty() ? cfg.deepseek.defaultModel : req.model},
        {"stream",      req.stream},
        {"temperature", req.temperature},
    };
    if (req.maxTokens > 0) body["max_tokens"] = req.maxTokens;

    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& m : req.messages) {
        nlohmann::json jm = {{"role", m.role}, {"content", m.content}};
        // assistant 携带 tool_calls
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
            // OpenAI 协议中：assistant 含 tool_calls 时 content 通常为空字符串或 null
            if (m.content.empty()) jm["content"] = nullptr;
        }
        // DeepSeek thinking 模型：上一轮 assistant 的 reasoning_content 选填回传。
        // M-1 (2026-05-24) probe_reasoning 实测：回传 / 剥离都返回 HTTP 200，
        // 两种形式服务端均接受。保留回传以兼容未来协议收紧（参见 chat_provider.h）。
        if (m.role == "assistant" && !m.reasoningContent.empty()) {
            jm["reasoning_content"] = m.reasoningContent;
        }
        // role=="tool" 必须带 tool_call_id（OpenAI 协议）
        if (m.role == "tool") {
            jm["tool_call_id"] = m.toolCallId;
            if (!m.toolName.empty()) jm["name"] = m.toolName;
        }
        msgs.push_back(std::move(jm));
    }
    body["messages"] = std::move(msgs);

    // === M4: tools / tool_choice ===
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

    const std::string payload = body.dump();

    XAI_LOG_INFO("DeepSeek POST {} model={} stream={} msgs={}",
                 url, body["model"].get<std::string>(), req.stream, req.messages.size());

    auto headers = buildHeaders(apiKey);

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
                cb.onError("DeepSeek HTTP " + std::to_string(r.status_code) + ": " + r.text);
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
                // 非流式 tool_calls
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
            if (cb.onDone) cb.onDone();
        } catch (const std::exception& e) {
            if (cb.onError) cb.onError(std::string("解析响应失败: ") + e.what());
        }
        return;
    }

    // 流式 SSE
    std::atomic_bool finished{false};
    std::string rawBuf;  // 缓存原始响应前 4KB，错误时回放

    // 流式 tool_calls 增量拼装：按 index 区分多个 call，每个的 id/name/arguments 分片到达。
    struct PartialToolCall {
        std::string id;
        std::string name;
        std::string argumentsJson;
    };
    // map<index, PartialToolCall>；最后按 index 升序导出
    std::map<int, PartialToolCall> partialCalls;
    bool sawToolCalls = false;
    std::string finishReason;

    SseParser parser([&](std::string_view data) {
        if (data == "[DONE]") {
            finished = true;
            // 先回吐 tool_calls（若有），再 onDone
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
            if (!j.contains("choices") || j["choices"].empty()) return;
            const auto& choice = j["choices"][0];
            if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
                finishReason = choice["finish_reason"].get<std::string>();
            }
            if (choice.contains("delta") && choice["delta"].is_object()) {
                const auto& d = choice["delta"];
                // DeepSeek 的 thinking / reasoner 模型有 reasoning_content 字段。
                // 必须与 content 分开输出：thinking 模型要求下一轮把它原样回传。
                if (d.contains("reasoning_content") && d["reasoning_content"].is_string()) {
                    if (cb.onReasoningDelta) cb.onReasoningDelta(d["reasoning_content"].get<std::string>());
                }
                if (d.contains("content") && d["content"].is_string()) {
                    if (cb.onDelta) cb.onDelta(d["content"].get<std::string>());
                }
                // === tool_calls 增量 ===
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
                                // name 一般完整一次到达，但允许累加防御性处理
                                if (p.name.empty()) p.name = f["name"].get<std::string>();
                                else                p.name += f["name"].get<std::string>();
                            }
                            if (f.contains("arguments") && f["arguments"].is_string()) {
                                p.argumentsJson += f["arguments"].get<std::string>();
                            }
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            XAI_LOG_WARN("DeepSeek SSE chunk parse error: {}; data={}",
                         e.what(), std::string(data));
        }
    });

    cpr::WriteCallback writer{[&](std::string_view data, intptr_t /*userdata*/) -> bool {
        // 缓存前 4KB 原始响应体，用于非 200 时错误诊断（SSE 模式下 cpr 不会自动填充 r.text）
        if (rawBuf.size() < 4096) {
            rawBuf.append(data.data(),
                          std::min<std::size_t>(data.size(), 4096 - rawBuf.size()));
        }
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
        const std::string& errBody = r.text.empty() ? rawBuf : r.text;
        XAI_LOG_ERROR("DeepSeek HTTP {}: {}", r.status_code, errBody);
        if (cb.onError) {
            cb.onError("DeepSeek HTTP " + std::to_string(r.status_code) + ": " + errBody);
        }
        return;
    }

    if (!finished && cb.onDone) {
        // 防御：连接断早了没收到 [DONE]，也把已拼装好的 tool_calls 回吐
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

}  // namespace x64ai
