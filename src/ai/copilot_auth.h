// ai/copilot_auth.h
//
// 复用本机 GitHub Copilot OAuth 凭据（由 VS Code / gh / OpenCode 写入），
// 并在用户没有这些工具时通过 GitHub Device Flow 自助登录。
//
// 持久化位置：%USERPROFILE%/.config/github-copilot/apps.json
//   {
//     "github.com:Iv1.b507a08c87ecfe98": {
//        "user": "<login>",
//        "oauth_token": "<gho_xxx>",
//        "githubAppId": "Iv1.b507a08c87ecfe98"
//     }
//   }
//
// chat token 通过 https://api.github.com/copilot_internal/v2/token
// 由 oauth_token 换取，约 30 分钟有效，过期前自动刷新。
#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace x64ai {

struct CopilotToken {
    std::string                                    token;
    std::chrono::system_clock::time_point          expiresAt;
};

// Device Flow 第一阶段返回。
struct DeviceCodeInfo {
    std::string deviceCode;
    std::string userCode;
    std::string verificationUri;
    int         intervalSec   = 5;
    int         expiresInSec  = 900;
};

// Device Flow 轮询结果。
enum class DevicePollState {
    Pending,           // 用户尚未授权，继续等
    SlowDown,          // 服务器要求加大轮询间隔
    Authorized,        // 成功，access_token 已就绪
    ExpiredToken,      // device_code 过期
    AccessDenied,      // 用户拒绝
    NetworkError,      // HTTP/网络错误
    UnexpectedError    // 其它
};

struct DevicePollResult {
    DevicePollState state = DevicePollState::Pending;
    std::string     accessToken;   // state==Authorized 时有效
    std::string     errorMessage;  // 任意非 Pending 的诊断信息
};

class CopilotAuth {
public:
    static CopilotAuth& instance();

    // ---------- 已登录态查询 ----------

    // 读取磁盘上的 OAuth token；返回空表示未登录。
    std::optional<std::string> readOAuthToken();

    // 已登录的 GitHub 用户名（apps.json 里的 user 字段）。
    std::optional<std::string> currentUserLogin();

    // 取一个有效的 chat token（必要时刷新）。失败返回 nullopt。
    std::optional<CopilotToken> getChatToken();

    // 强制清空内存缓存的 chat token。
    void invalidate();

    // 删除 apps.json 中本插件写入的条目（其他工具的不动）。
    // 同时清空 chat token 缓存。
    bool logout();

    // ---------- Device Flow ----------

    // 调 https://github.com/login/device/code，使用 GitHub CLI 公共 client_id。
    std::optional<DeviceCodeInfo> beginDeviceLogin();

    // 调 https://github.com/login/oauth/access_token 进行一次轮询。
    DevicePollResult pollDeviceLogin(const std::string& deviceCode);

    // 把 access_token 落盘为 apps.json 兼容格式，并尝试拉取用户名。
    // 返回 true 表示成功写入。
    bool persistOAuthToken(const std::string& accessToken);

private:
    CopilotAuth() = default;

    std::optional<CopilotToken> exchangeChatToken(const std::string& oauthToken);

    // 通过 https://api.github.com/user 拿 login（写 apps.json 用）。
    std::optional<std::string> fetchGithubLogin(const std::string& accessToken);

    std::mutex                  mtx_;
    std::optional<CopilotToken> cached_;
};

}  // namespace x64ai
