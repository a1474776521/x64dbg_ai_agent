// ai/copilot_auth.cpp
#include "ai/copilot_auth.h"

#include <fstream>
#include <sstream>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "util/config.h"
#include "util/http_options.h"
#include "util/logging.h"
#include "util/paths.h"

namespace x64ai {

namespace {

// VSCode GitHub Copilot Chat 扩展使用的公共 OAuth App client_id（公开值）。
// 走该 App 拿到的 access_token 才能换 Copilot chat token；
// gh CLI 的 client_id (178c6fc778ccc68e1d6a) 拿到的 gho_ token 调
// copilot_internal/v2/token 会 404。
constexpr const char* kDeviceClientId = "01ab8ac9400c4e429b23";
// VSCode Copilot 走 device flow 时不传 scope（让 GitHub 按 App 默认 scope 处理）。
// 传 read:user 等普通 scope 反而会被这个 App 拒绝。
constexpr const char* kDeviceScopes   = "";

constexpr const char* kDeviceCodeUrl  = "https://github.com/login/device/code";
constexpr const char* kDeviceTokenUrl = "https://github.com/login/oauth/access_token";
constexpr const char* kGithubUserUrl  = "https://api.github.com/user";

// 我们写入 apps.json 时使用的 key/githubAppId。
// VSCode Copilot 用的是 Iv1.b507a08c87ecfe98；为了兼容 readOAuthToken 的扫描逻辑
// （遍历对象寻找 oauth_token 即可），我们用一个独立 key 标识来源是本插件。
constexpr const char* kAppsJsonKey       = "github.com:x64dbg-ai-plugin";
constexpr const char* kAppsJsonGithubAppId = "Iv1.b507a08c87ecfe98";

cpr::Header copilotEditorHeaders()
{
    const auto& cfg = Config::instance().get();
    return {
        {"Accept",                "application/json"},
        {"Editor-Version",        cfg.copilot.editorVersion},
        {"Editor-Plugin-Version", cfg.copilot.editorPluginVersion},
        {"User-Agent",            cfg.copilot.userAgent},
    };
}

}  // namespace

CopilotAuth& CopilotAuth::instance()
{
    static CopilotAuth inst;
    return inst;
}

void CopilotAuth::invalidate()
{
    std::lock_guard<std::mutex> lk(mtx_);
    cached_.reset();
}

std::optional<std::string> CopilotAuth::readOAuthToken()
{
    namespace fs = std::filesystem;

    const fs::path apps = copilotAuthDir() / "apps.json";
    std::error_code ec;
    if (!fs::exists(apps, ec)) {
        XAI_LOG_WARN("apps.json not found: {}", apps.string());
        return std::nullopt;
    }

    std::ifstream ifs(apps);
    if (!ifs) {
        XAI_LOG_WARN("cannot open apps.json: {}", apps.string());
        return std::nullopt;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();

    try {
        auto j = nlohmann::json::parse(ss.str(), nullptr, true, true);
        // 优先返回本插件写入的条目
        if (j.is_object() && j.contains(kAppsJsonKey)) {
            const auto& entry = j[kAppsJsonKey];
            if (entry.is_object() && entry.contains("oauth_token")) {
                return entry["oauth_token"].get<std::string>();
            }
        }
        // 否则扫描任意条目（兼容 gh / VSCode 已登录）
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_object() && it.value().contains("oauth_token")) {
                return it.value()["oauth_token"].get<std::string>();
            }
        }
    } catch (const std::exception& e) {
        XAI_LOG_ERROR("apps.json parse error: {}", e.what());
    }
    return std::nullopt;
}

std::optional<std::string> CopilotAuth::currentUserLogin()
{
    namespace fs = std::filesystem;

    const fs::path apps = copilotAuthDir() / "apps.json";
    std::error_code ec;
    if (!fs::exists(apps, ec)) return std::nullopt;

    std::ifstream ifs(apps);
    if (!ifs) return std::nullopt;
    std::stringstream ss;
    ss << ifs.rdbuf();

    try {
        auto j = nlohmann::json::parse(ss.str(), nullptr, true, true);
        if (!j.is_object()) return std::nullopt;
        // 先看本插件条目
        if (j.contains(kAppsJsonKey)) {
            const auto& e2 = j[kAppsJsonKey];
            if (e2.is_object() && e2.contains("user") && e2["user"].is_string()) {
                return e2["user"].get<std::string>();
            }
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_object() && it.value().contains("user")
                && it.value()["user"].is_string()) {
                return it.value()["user"].get<std::string>();
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<CopilotToken> CopilotAuth::exchangeChatToken(const std::string& oauthToken)
{
    const auto& cfg = Config::instance().get();

    cpr::Header headers{
        {"Authorization",         "token " + oauthToken},
        {"Accept",                "application/json"},
        {"Editor-Version",        cfg.copilot.editorVersion},
        {"Editor-Plugin-Version", cfg.copilot.editorPluginVersion},
        {"User-Agent",            cfg.copilot.userAgent},
    };

    XAI_LOG_DEBUG("exchanging chat token via {}", cfg.copilot.tokenEndpoint);
    cpr::Response r = cpr::Get(
        cpr::Url{cfg.copilot.tokenEndpoint},
        headers,
        defaultSslOptions(),
        cpr::Timeout{cfg.httpTimeoutMs});

    if (r.error) {
        XAI_LOG_ERROR("token exchange network error: {}", r.error.message);
        return std::nullopt;
    }
    if (r.status_code < 200 || r.status_code >= 300) {
        XAI_LOG_ERROR("token exchange http {}: {}", r.status_code, r.text);
        return std::nullopt;
    }

    try {
        auto j = nlohmann::json::parse(r.text);
        if (!j.contains("token")) {
            XAI_LOG_ERROR("token exchange response missing 'token': {}", r.text);
            return std::nullopt;
        }
        CopilotToken tok;
        tok.token = j["token"].get<std::string>();

        if (j.contains("expires_at") && j["expires_at"].is_number_integer()) {
            const auto secs = j["expires_at"].get<int64_t>();
            tok.expiresAt = std::chrono::system_clock::time_point{std::chrono::seconds{secs}};
        } else {
            tok.expiresAt = std::chrono::system_clock::now() + std::chrono::minutes(25);
        }
        XAI_LOG_INFO("chat token acquired (len={})", tok.token.size());
        return tok;
    } catch (const std::exception& e) {
        XAI_LOG_ERROR("token exchange parse error: {}; body={}", e.what(), r.text);
        return std::nullopt;
    }
}

std::optional<CopilotToken> CopilotAuth::getChatToken()
{
    std::lock_guard<std::mutex> lk(mtx_);

    const auto now = std::chrono::system_clock::now();
    if (cached_ && cached_->expiresAt - std::chrono::seconds(60) > now) {
        return cached_;
    }

    auto oauth = readOAuthToken();
    if (!oauth) return std::nullopt;

    auto fresh = exchangeChatToken(*oauth);
    if (!fresh) return std::nullopt;

    cached_ = fresh;
    return cached_;
}

// ============================================================
// Device Flow
// ============================================================

std::optional<DeviceCodeInfo> CopilotAuth::beginDeviceLogin()
{
    cpr::Header headers = copilotEditorHeaders();
    cpr::Payload body{
        {"client_id", kDeviceClientId},
    };
    if (kDeviceScopes && kDeviceScopes[0] != '\0') {
        body.Add({"scope", kDeviceScopes});
    }

    cpr::Response r = cpr::Post(
        cpr::Url{kDeviceCodeUrl},
        headers,
        body,
        defaultSslOptions(),
        cpr::Timeout{Config::instance().get().httpTimeoutMs});

    if (r.error) {
        XAI_LOG_ERROR("device/code network error: {}", r.error.message);
        return std::nullopt;
    }
    if (r.status_code < 200 || r.status_code >= 300) {
        XAI_LOG_ERROR("device/code http {}: {}", r.status_code, r.text);
        return std::nullopt;
    }

    try {
        auto j = nlohmann::json::parse(r.text);
        DeviceCodeInfo info;
        info.deviceCode      = j.value("device_code",      "");
        info.userCode        = j.value("user_code",        "");
        info.verificationUri = j.value("verification_uri", "https://github.com/login/device");
        info.intervalSec     = j.value("interval",         5);
        info.expiresInSec    = j.value("expires_in",       900);
        if (info.deviceCode.empty() || info.userCode.empty()) {
            XAI_LOG_ERROR("device/code missing fields: {}", r.text);
            return std::nullopt;
        }
        XAI_LOG_INFO("device login started, user_code={} expires_in={}s",
                     info.userCode, info.expiresInSec);
        return info;
    } catch (const std::exception& e) {
        XAI_LOG_ERROR("device/code parse error: {}; body={}", e.what(), r.text);
        return std::nullopt;
    }
}

DevicePollResult CopilotAuth::pollDeviceLogin(const std::string& deviceCode)
{
    DevicePollResult res;

    cpr::Header headers = copilotEditorHeaders();
    cpr::Payload body{
        {"client_id",   kDeviceClientId},
        {"device_code", deviceCode},
        {"grant_type",  "urn:ietf:params:oauth:grant-type:device_code"},
    };

    cpr::Response r = cpr::Post(
        cpr::Url{kDeviceTokenUrl},
        headers,
        body,
        defaultSslOptions(),
        cpr::Timeout{Config::instance().get().httpTimeoutMs});

    if (r.error) {
        res.state = DevicePollState::NetworkError;
        res.errorMessage = r.error.message;
        return res;
    }

    try {
        auto j = nlohmann::json::parse(r.text);
        if (j.contains("access_token") && j["access_token"].is_string()) {
            res.state = DevicePollState::Authorized;
            res.accessToken = j["access_token"].get<std::string>();
            return res;
        }
        const std::string err = j.value("error", "");
        if (err == "authorization_pending") {
            res.state = DevicePollState::Pending;
        } else if (err == "slow_down") {
            res.state = DevicePollState::SlowDown;
        } else if (err == "expired_token") {
            res.state = DevicePollState::ExpiredToken;
            res.errorMessage = "device_code 已过期，请重新登录";
        } else if (err == "access_denied") {
            res.state = DevicePollState::AccessDenied;
            res.errorMessage = "用户拒绝授权";
        } else {
            res.state = DevicePollState::UnexpectedError;
            res.errorMessage = err.empty() ? r.text : err;
        }
    } catch (const std::exception& e) {
        res.state = DevicePollState::UnexpectedError;
        res.errorMessage = std::string("parse: ") + e.what() + "; body=" + r.text;
    }
    return res;
}

std::optional<std::string> CopilotAuth::fetchGithubLogin(const std::string& accessToken)
{
    cpr::Header headers = copilotEditorHeaders();
    headers["Authorization"] = "token " + accessToken;

    cpr::Response r = cpr::Get(
        cpr::Url{kGithubUserUrl},
        headers,
        defaultSslOptions(),
        cpr::Timeout{Config::instance().get().httpTimeoutMs});
    if (r.error || r.status_code < 200 || r.status_code >= 300) {
        XAI_LOG_WARN("fetchGithubLogin http {}: {}", r.status_code, r.text);
        return std::nullopt;
    }
    try {
        auto j = nlohmann::json::parse(r.text);
        if (j.contains("login") && j["login"].is_string()) {
            return j["login"].get<std::string>();
        }
    } catch (...) {
    }
    return std::nullopt;
}

bool CopilotAuth::persistOAuthToken(const std::string& accessToken)
{
    namespace fs = std::filesystem;

    auto loginOpt = fetchGithubLogin(accessToken);
    const std::string login = loginOpt.value_or("user");

    const fs::path dir  = copilotAuthDir();
    const fs::path apps = dir / "apps.json";

    nlohmann::json root = nlohmann::json::object();
    if (fs::exists(apps)) {
        try {
            std::ifstream ifs(apps);
            std::stringstream ss; ss << ifs.rdbuf();
            root = nlohmann::json::parse(ss.str(), nullptr, true, true);
            if (!root.is_object()) root = nlohmann::json::object();
        } catch (const std::exception& e) {
            XAI_LOG_WARN("apps.json parse failed, will overwrite: {}", e.what());
            root = nlohmann::json::object();
        }
    }

    nlohmann::json entry = nlohmann::json::object();
    entry["user"]         = login;
    entry["oauth_token"]  = accessToken;
    entry["githubAppId"]  = kAppsJsonGithubAppId;
    root[kAppsJsonKey]    = entry;

    try {
        std::ofstream ofs(apps, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            XAI_LOG_ERROR("cannot open apps.json for write: {}", apps.string());
            return false;
        }
        const std::string text = root.dump(2);
        ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
    } catch (const std::exception& e) {
        XAI_LOG_ERROR("apps.json write error: {}", e.what());
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        cached_.reset();
    }
    XAI_LOG_INFO("persisted oauth token for user={}", login);
    return true;
}

bool CopilotAuth::logout()
{
    namespace fs = std::filesystem;
    const fs::path apps = copilotAuthDir() / "apps.json";

    invalidate();

    if (!fs::exists(apps)) return true;

    try {
        nlohmann::json root;
        {
            std::ifstream ifs(apps);
            std::stringstream ss; ss << ifs.rdbuf();
            root = nlohmann::json::parse(ss.str(), nullptr, true, true);
        }
        if (!root.is_object()) return true;
        if (root.contains(kAppsJsonKey)) {
            root.erase(kAppsJsonKey);
        } else {
            // 兼容旧用户：清掉所有 oauth_token 条目（保守起见仅在仅有本插件场景下）
            // 这里不动其他工具的 token，避免破坏 gh/VSCode。
        }
        std::ofstream ofs(apps, std::ios::binary | std::ios::trunc);
        const std::string text = root.dump(2);
        ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
    } catch (const std::exception& e) {
        XAI_LOG_ERROR("logout write error: {}", e.what());
        return false;
    }
    return true;
}

}  // namespace x64ai
