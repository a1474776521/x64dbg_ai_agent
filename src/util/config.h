// util/config.h
//
// 插件配置加载器（读取 %APPDATA%/x64dbg-ai-plugin/config.json）。
// 提供 Copilot 端点、Editor 头部伪装、默认模型等可覆盖项。
// 缺省值与 GitHub Copilot CLI 客户端保持一致，便于直接复用本机 OAuth。
#pragma once

#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace x64ai {

struct CopilotEndpointConfig {
    // Chat / Models / Embeddings 基础 URL
    std::string apiBase = "https://api.githubcopilot.com";
    // OAuth -> Chat token 交换端点
    std::string tokenEndpoint = "https://api.github.com/copilot_internal/v2/token";

    // Editor-* 头：伪装为 VSCode Copilot Chat 客户端（与 device flow client_id 同源）。
    // copilot-cli / GitHubCopilotCLI 这些值在 /models 端点会被拒（unknown Copilot-Integration-Id）。
    std::string editorVersion        = "vscode/1.95.0";
    std::string editorPluginVersion  = "copilot-chat/0.22.0";
    std::string copilotIntegrationId = "vscode-chat";
    std::string userAgent            = "GitHubCopilotChat/0.22.0";
    std::string openaiIntent         = "conversation-other";
};

struct DeepSeekEndpointConfig {
    // OpenAI 兼容的基础 URL（chat/completions、models）
    std::string apiBase = "https://api.deepseek.com/v1";
    // 默认模型
    std::string defaultModel = "deepseek-chat";
};

struct AppConfig {
    CopilotEndpointConfig copilot;
    DeepSeekEndpointConfig deepseek;
    // 当前 Provider："copilot" | "deepseek"
    std::string           provider     = "copilot";
    std::string           defaultModel = "gpt-4o-mini";
    int                   httpTimeoutMs = 60000;
    // 流式响应超时（毫秒）：用于 chat/completions 的 SSE 长连接。
    // DeepSeek reasoner / 大模型推理可能持续几分钟，默认 10 分钟。
    int                   streamTimeoutMs = 600000;
    // 流式低速阈值（秒）：连续 N 秒未收到任何字节即视为断流并中断。
    // 防止真正的网络挂死把整条请求拖到 streamTimeoutMs 才退出。
    int                   streamLowSpeedSec = 30;
};

class Config {
public:
    static Config& instance();

    // 首次访问时自动加载；可通过 reload() 强制重新读取。
    const AppConfig& get();
    void             reload();

private:
    Config() = default;
    void loadLocked();

    std::mutex mtx_;
    bool       loaded_ = false;
    AppConfig  data_;
};

}  // namespace x64ai
