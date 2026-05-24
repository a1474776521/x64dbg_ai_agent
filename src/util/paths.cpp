#include "paths.h"

#include <cstdlib>
#include <stdexcept>

namespace x64ai {

namespace fs = std::filesystem;

namespace {

fs::path getEnvPath(const char* name) {
    char* val = nullptr;
    size_t len = 0;
    if (_dupenv_s(&val, &len, name) != 0 || val == nullptr) {
        throw std::runtime_error(std::string("env var not set: ") + name);
    }
    fs::path p(val);
    free(val);
    return p;
}

void ensureDir(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
}

}  // namespace

fs::path pluginRootDir() {
    auto p = getEnvPath("APPDATA") / "x64dbg-ai-plugin";
    ensureDir(p);
    return p;
}

fs::path pluginConfigFile() {
    return pluginRootDir() / "config.json";
}

fs::path pluginLogDir() {
    auto p = pluginRootDir() / "logs";
    ensureDir(p);
    return p;
}

fs::path pluginProjectsDir() {
    auto p = pluginRootDir() / "projects";
    ensureDir(p);
    return p;
}

fs::path projectDbPath(const std::string& sha256Hex) {
    return pluginProjectsDir() / (sha256Hex + ".db");
}

fs::path copilotAuthDir() {
    auto p = getEnvPath("USERPROFILE") / ".config" / "github-copilot";
    ensureDir(p);
    return p;
}

}  // namespace x64ai
