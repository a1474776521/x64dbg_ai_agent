// ai/embedding_client.cpp
#include "ai/embedding_client.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "util/config.h"
#include "util/http_options.h"
#include "util/logging.h"
#include "util/secret_store.h"

namespace x64ai {

namespace {

// GitHub Models inference 端点（PAT Bearer，OpenAI 兼容）
constexpr const char* kModelsBaseUrl = "https://models.github.ai/inference";
constexpr const char* kEmbeddingModel = "openai/text-embedding-3-small";

}  // namespace

EmbeddingClient& EmbeddingClient::instance()
{
    static EmbeddingClient inst;
    return inst;
}

std::string EmbeddingClient::resolveToken()
{
    auto tok = SecretStore::instance().resolveSecret("github_pat", "github_pat");
    return tok ? *tok : std::string{};
}

std::vector<float> EmbeddingClient::embed(const std::string& text)
{
    auto v = embedBatch({text});
    if (v.empty()) return {};
    return std::move(v[0]);
}

std::vector<std::vector<float>> EmbeddingClient::embedBatch(
    const std::vector<std::string>& texts)
{
    std::vector<std::vector<float>> out(texts.size());
    if (texts.empty()) return out;

    auto token = resolveToken();
    if (token.empty()) {
        XAI_LOG_ERROR("embedBatch: 未配置 github_pat（SecretStore/config.json 都没有）");
        return out;
    }

    const auto& cfg = Config::instance().get();

    nlohmann::json body = {
        {"model", kEmbeddingModel},
        {"input", texts},
    };

    cpr::Header headers{
        {"Authorization", "Bearer " + token},
        {"Accept",        "application/json"},
        {"Content-Type",  "application/json"},
        {"User-Agent",    cfg.copilot.userAgent},
    };

    cpr::Response r = cpr::Post(
        cpr::Url{std::string(kModelsBaseUrl) + "/embeddings"},
        headers,
        cpr::Body{body.dump()},
        defaultSslOptions(),
        cpr::Timeout{cfg.httpTimeoutMs});

    if (r.error || r.status_code < 200 || r.status_code >= 300) {
        XAI_LOG_ERROR("embeddings http {}: {}",
                      r.status_code, r.error ? r.error.message : r.text);
        return out;
    }

    try {
        auto j = nlohmann::json::parse(r.text);
        if (!j.contains("data") || !j["data"].is_array()) return out;
        // OpenAI 风格响应：data 数组按 index 排序
        for (const auto& item : j["data"]) {
            if (!item.contains("index") || !item.contains("embedding")) continue;
            int idx = item["index"].get<int>();
            const auto& emb = item["embedding"];
            if (!emb.is_array() || idx < 0 || idx >= static_cast<int>(out.size())) continue;
            std::vector<float> v;
            v.reserve(emb.size());
            for (const auto& x : emb) v.push_back(x.get<float>());
            out[idx] = std::move(v);
        }
    } catch (const std::exception& e) {
        XAI_LOG_ERROR("embeddings parse error: {}", e.what());
        return out;
    }
    return out;
}

}  // namespace x64ai
