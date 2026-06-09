// ai/kspmas_chat_client.cpp
//
// 金山云 KSPmas chat client。结构与 DeepSeekChatClient 一一对应；只删了 thinking
// 模型相关的 reasoning_content 字段（KSPmas 不一定支持，避免被服务端 400）。
#include "ai/kspmas_chat_client.h"

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

constexpr const char* kSecretName = "kspmas_api_key";

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

KSPmasChatClient& KSPmasChatClient::instance()
{
    static KSPmasChatClient inst;
    return inst;
}

// ====== Key 管理 ======

bool KSPmasChatClient::saveApiKey(const std::string& key)
{
    if (key.empty()) return false;
    bool ok = SecretStore::instance().saveSecret(kSecretName, key);
    XAI_LOG_INFO("KSPmas: saveApiKey ok={}", ok);
    return ok;
}

bool KSPmasChatClient::clearApiKey()
{
    bool ok = SecretStore::instance().deleteSecret(kSecretName);
    XAI_LOG_INFO("KSPmas: clearApiKey ok={}", ok);
    return ok;
}

std::optional<std::string> KSPmasChatClient::loadApiKey() const
{
    return SecretStore::instance().loadSecret(kSecretName);
}

std::string KSPmasChatClient::maskedApiKey() const
{
    auto k = loadApiKey();
    if (!k || k->empty()) return {};
    const std::string& s = *k;
    if (s.size() <= 12) return std::string(s.size(), '*');
    return s.substr(0, 8) + "****" + s.substr(s.size() - 4);
}

bool KSPmasChatClient::isAuthenticated(std::string* outReason) const
{
    auto k = loadApiKey();
    if (!k || k->empty()) {
        if (outReason) *outReason = "未设置 KSPmas API Key";
        return false;
    }
    return true;
}

// ====== /models ======

std::vector<std::string> KSPmasChatClient::listModels()
{
    auto keyOpt = loadApiKey();
    if (!keyOpt || keyOpt->empty()) {
        XAI_LOG_WARN("KSPmas listModels: no api key");
        return {};
    }

    const auto& cfg = Config::instance().get();
    const std::string url = cfg.kspmas.apiBase + "/models";

    cpr::Response r = cpr::Get(
        cpr::Url{url}, buildHeaders(*keyOpt),
        defaultSslOptions(),
        cpr::Timeout{cfg.httpTimeoutMs});

    if (r.error || r.status_code < 200 || r.status_code >= 300) {
        XAI_LOG_ERROR("KSPmas listModels http {}: {}", r.status_code,
                      r.error ? r.error.message : r.text);
        // 不致命：返回空让 UI 兜底用 defaultModel()
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
        XAI_LOG_ERROR("KSPmas listModels parse error: {}", e.what());
    }
    XAI_LOG_INFO("KSPmas listModels -> {} entries", ids.size());
    return ids;
}

// ====== /chat/completions ======

void KSPmasChatClient::streamChat(const ChatRequest& req, const ChatStreamCallbacks& cb)
{
    auto keyOpt = loadApiKey();
    if (!keyOpt || keyOpt->empty()) {
        if (cb.onError) cb.onError("未设置 KSPmas API Key（请点登录设置）");
        return;
    }
    const std::string apiKey = *keyOpt;

    const auto& cfg = Config::instance().get();
    const std::string url = cfg.kspmas.apiBase + "/chat/completions";

    nlohmann::json body = {
        {"model",       req.model.empty() ? cfg.kspmas.defaultModel : req.model},
        {"stream",      req.stream},
        {"temperature", req.temperature},
    };
    if (req.maxTokens > 0) body["max_tokens"] = req.maxTokens;
    // 与 DeepSeek 一致：流式末尾要 usage chunk；若 KSPmas 不识别此字段，OpenAI 兼容服务
    // 一般会安全忽略未知字段。
    if (req.stream) {
        body["stream_options"] = {{"include_usage", true}};
    }

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
            // OpenAI 协议：assistant 含 tool_calls 时 content 通常为空字符串或 null
            if (m.content.empty()) jm["content"] = nullptr;
        }
        // 注意：相比 DeepSeek 客户端，这里**不回传 reasoning_content**。
        // KSPmas 不一定支持 thinking 模型，回传可能被服务器报 400 "unknown field"。
        // role=="tool" 必须带 tool_call_id（OpenAI 协议）
        if (m.role == "tool") {
            jm["tool_call_id"] = m.toolCallId;
            if (!m.toolName.empty()) jm["name"] = m.toolName;
        }
        msgs.push_back(std::move(jm));
    }
    body["messages"] = std::move(msgs);

    // === tools / tool_choice ===
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

    XAI_LOG_INFO("KSPmas POST {} model={} stream={} msgs={}",
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
                cb.onError("KSPmas HTTP " + std::to_string(r.status_code) + ": " + r.text);
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
            if (j.contains("usage") && j["usage"].is_object() && cb.onUsage) {
                const auto& u = j["usage"];
                UsageInfo ui;
                ui.promptTokens        = u.value("prompt_tokens", 0);
                ui.completionTokens    = u.value("completion_tokens", 0);
                ui.totalTokens         = u.value("total_tokens", 0);
                ui.cachedPromptTokens  = u.value("prompt_cache_hit_tokens", 0);
                cb.onUsage(ui);
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
            // usage chunk（KSPmas 是否发未知；OpenAI 兼容服务一般在 [DONE] 前发一个 choices=[] 带 usage）
            if (j.contains("usage") && j["usage"].is_object() && cb.onUsage) {
                const auto& u = j["usage"];
                UsageInfo ui;
                ui.promptTokens        = u.value("prompt_tokens", 0);
                ui.completionTokens    = u.value("completion_tokens", 0);
                ui.totalTokens         = u.value("total_tokens", 0);
                ui.cachedPromptTokens  = u.value("prompt_cache_hit_tokens", 0);
                cb.onUsage(ui);
            }
            if (!j.contains("choices") || j["choices"].empty()) return;
            const auto& choice = j["choices"][0];
            if (choice.contains("delta") && choice["delta"].is_object()) {
                const auto& d = choice["delta"];
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
            XAI_LOG_WARN("KSPmas SSE chunk parse error: {}; data={}",
                         e.what(), std::string(data));
        }
    });

    cpr::WriteCallback writer{[&](std::string_view data, intptr_t /*userdata*/) -> bool {
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
        XAI_LOG_ERROR("KSPmas HTTP {}: {}", r.status_code, errBody);
        if (cb.onError) {
            cb.onError("KSPmas HTTP " + std::to_string(r.status_code) + ": " + errBody);
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
