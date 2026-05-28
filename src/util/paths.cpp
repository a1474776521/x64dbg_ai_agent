#include "paths.h"

#include <cstdlib>
#include <stdexcept>

#include "util/encoding.h"

namespace x64ai {

namespace fs = std::filesystem;

namespace {

// 用 _wdupenv_s 读 wchar 环境变量，避免中文用户名 / 中文 APPDATA 路径
// 被 _dupenv_s（ACP）截断或乱码。
fs::path getEnvPath(const wchar_t* name) {
    wchar_t* val = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&val, &len, name) != 0 || val == nullptr) {
        // 抛错时把 name 转成 UTF-8 拼到消息里
        std::string n = wideToUtf8(name);
        throw std::runtime_error(std::string("env var not set: ") + n);
    }
    fs::path p(val);  // wstring 构造，路径精确
    free(val);
    return p;
}

void ensureDir(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
}

}  // namespace

fs::path pluginRootDir() {
    auto p = getEnvPath(L"APPDATA") / L"x64dbg-ai-plugin";
    ensureDir(p);
    return p;
}

fs::path pluginConfigFile() {
    return pluginRootDir() / L"config.json";
}

fs::path pluginLogDir() {
    auto p = pluginRootDir() / L"logs";
    ensureDir(p);
    return p;
}

fs::path pluginProjectsDir() {
    auto p = pluginRootDir() / L"projects";
    ensureDir(p);
    return p;
}

fs::path projectDbPath(const std::string& sha256Hex) {
    // sha256Hex 是 ASCII，直接用即可
    return pluginProjectsDir() / (sha256Hex + ".db");
}

fs::path pluginScriptsDir() {
    auto p = pluginRootDir() / L"scripts";
    ensureDir(p);
    return p;
}

fs::path copilotAuthDir() {
    auto p = getEnvPath(L"USERPROFILE") / L".config" / L"github-copilot";
    ensureDir(p);
    return p;
}

}  // namespace x64ai
