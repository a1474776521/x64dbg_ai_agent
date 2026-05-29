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

    // extra_dbg_cmd_whitelist：用户在 config.json 自定义追加到 run_dbg_command 白名单
    if (j.contains("extra_dbg_cmd_whitelist") && j["extra_dbg_cmd_whitelist"].is_array()) {
        data_.extraDbgCmdWhitelist.clear();
        for (const auto& v : j["extra_dbg_cmd_whitelist"]) {
            if (!v.is_string()) continue;
            std::string s = v.get<std::string>();
            // 转小写存储，省得每次比较再转
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (s.empty() || s.size() > 32) continue;  // sanity
            data_.extraDbgCmdWhitelist.push_back(std::move(s));
        }
        XAI_LOG_INFO("config: extra_dbg_cmd_whitelist loaded ({} entries)",
                     data_.extraDbgCmdWhitelist.size());
    }

    // K-33：auto_approve_tools 自动批准列表（仍写 audit）
    // 注：confirm_policy.cpp 在 isAutoApproved() 中再做一次黑名单过滤——这里只做基本清洗。
    if (j.contains("auto_approve_tools") && j["auto_approve_tools"].is_array()) {
        data_.autoApproveTools.clear();
        for (const auto& v : j["auto_approve_tools"]) {
            if (!v.is_string()) continue;
            std::string s = v.get<std::string>();
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (s.empty() || s.size() > 64) continue;
            data_.autoApproveTools.push_back(std::move(s));
        }
        XAI_LOG_INFO("config: auto_approve_tools loaded ({} entries)",
                     data_.autoApproveTools.size());
    }

    // K-35：agent 编排增强开关
    readField(j, "tool_retry_enabled",      data_.toolRetryEnabled);
    readField(j, "tool_retry_max",          data_.toolRetryMax);
    readField(j, "auto_rag_inject_enabled", data_.autoRagInjectEnabled);
    readField(j, "auto_rag_top_k",          data_.autoRagTopK);
    // 范围钳制（防 config 写出离谱值导致刷接口/刷配额）
    if (data_.toolRetryMax < 0) data_.toolRetryMax = 0;
    if (data_.toolRetryMax > 3) data_.toolRetryMax = 3;
    if (data_.autoRagTopK   < 1) data_.autoRagTopK  = 1;
    if (data_.autoRagTopK   > 16) data_.autoRagTopK = 16;
    XAI_LOG_INFO("config: K-35 tool_retry={}(max={}) auto_rag_inject={}(top_k={})",
                 data_.toolRetryEnabled, data_.toolRetryMax,
                 data_.autoRagInjectEnabled, data_.autoRagTopK);

    XAI_LOG_INFO("config loaded: provider={}, copilot.api_base={}, deepseek.api_base={}, default_model={}",
                 data_.provider, data_.copilot.apiBase, data_.deepseek.apiBase, data_.defaultModel);
}

}  // namespace x64ai
