// util/config.cpp
#include "util/config.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "util/logging.h"
#include "util/paths.h"

namespace x64ai {

namespace fs = std::filesystem;

Config& Config::instance()
{
    static Config inst;
    return inst;
}

const AppConfig& Config::get()
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!loaded_) loadLocked();
    return data_;
}

void Config::reload()
{
    std::lock_guard<std::mutex> lk(mtx_);
    loaded_ = false;
    loadLocked();
}

namespace {

template <typename T>
void readField(const nlohmann::json& j, const char* key, T& out)
{
    if (!j.contains(key)) return;
    try {
        out = j.at(key).get<T>();
    } catch (const std::exception& e) {
        XAI_LOG_WARN("config field '{}' invalid: {}", key, e.what());
    }
}

}  // namespace

void Config::loadLocked()
{
    data_   = AppConfig{};  // 重置为默认
    loaded_ = true;

    const fs::path cfg = pluginConfigFile();
    std::error_code ec;
    if (!fs::exists(cfg, ec)) {
        XAI_LOG_INFO("config.json not found, using defaults: {}", cfg.string());
        return;
    }

    std::ifstream ifs(cfg);
    if (!ifs) {
        XAI_LOG_WARN("cannot open config.json: {}", cfg.string());
        return;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ss.str(), nullptr, true, true);
    } catch (const std::exception& e) {
        XAI_LOG_ERROR("config.json parse error: {}", e.what());
        return;
    }

    if (j.contains("copilot") && j["copilot"].is_object()) {
        const auto& c = j["copilot"];
        readField(c, "api_base",              data_.copilot.apiBase);
        readField(c, "token_endpoint",        data_.copilot.tokenEndpoint);
        readField(c, "editor_version",        data_.copilot.editorVersion);
        readField(c, "editor_plugin_version", data_.copilot.editorPluginVersion);
        readField(c, "integration_id",        data_.copilot.copilotIntegrationId);
        readField(c, "user_agent",            data_.copilot.userAgent);
        readField(c, "openai_intent",         data_.copilot.openaiIntent);
    }

    if (j.contains("deepseek") && j["deepseek"].is_object()) {
        const auto& d = j["deepseek"];
        readField(d, "api_base",      data_.deepseek.apiBase);
        readField(d, "default_model", data_.deepseek.defaultModel);
    }

    readField(j, "provider",        data_.provider);
    readField(j, "default_model",   data_.defaultModel);
    readField(j, "http_timeout_ms",      data_.httpTimeoutMs);
    readField(j, "stream_timeout_ms",    data_.streamTimeoutMs);
    readField(j, "stream_low_speed_sec", data_.streamLowSpeedSec);

    XAI_LOG_INFO("config loaded: provider={}, copilot.api_base={}, deepseek.api_base={}, default_model={}",
                 data_.provider, data_.copilot.apiBase, data_.deepseek.apiBase, data_.defaultModel);
}

}  // namespace x64ai
