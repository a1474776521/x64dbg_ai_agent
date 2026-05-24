// ai/copilot_chat_client.cpp
#include "ai/copilot_chat_client.h"

#include <atomic>

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

    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& m : req.messages) {
        msgs.push_back({{"role", m.role}, {"content", m.content}});
    }
    body["messages"] = std::move(msgs);

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
                const auto& msg = j["choices"][0]["message"];
                if (msg.contains("content") && cb.onDelta) {
                    cb.onDelta(msg["content"].get<std::string>());
                }
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

    SseParser parser([&](std::string_view data) {
        if (data == "[DONE]") {
            finished = true;
            if (cb.onDone) cb.onDone();
            return;
        }
        try {
            auto j = nlohmann::json::parse(data);
            if (!j.contains("choices") || j["choices"].empty()) return;
            const auto& choice = j["choices"][0];
            if (choice.contains("delta") && choice["delta"].is_object()) {
                const auto& d = choice["delta"];
                if (d.contains("content") && d["content"].is_string()) {
                    if (cb.onDelta) cb.onDelta(d["content"].get<std::string>());
                }
            } else if (choice.contains("message") && choice["message"].is_object()) {
                const auto& m = choice["message"];
                if (m.contains("content") && m["content"].is_string()) {
                    if (cb.onDelta) cb.onDelta(m["content"].get<std::string>());
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

    if (!finished && cb.onDone) cb.onDone();
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
