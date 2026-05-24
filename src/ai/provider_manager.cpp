// ai/provider_manager.cpp
#include "ai/provider_manager.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "ai/copilot_chat_client.h"
#include "ai/deepseek_chat_client.h"
#include "util/config.h"
#include "util/logging.h"
#include "util/paths.h"

namespace x64ai {

namespace fs = std::filesystem;

namespace {
fs::path providerStateFile()
{
    return pluginRootDir() / "provider.txt";
}
}

ProviderManager& ProviderManager::instance()
{
    static ProviderManager inst;
    return inst;
}

IChatProvider* ProviderManager::get(ProviderKind kind)
{
    switch (kind) {
    case ProviderKind::Copilot:  return &CopilotChatClient::instance();
    case ProviderKind::DeepSeek: return &DeepSeekChatClient::instance();
    }
    return &CopilotChatClient::instance();
}

void ProviderManager::initIfNeeded()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (initialized_) return;
    initialized_ = true;
    loadFromDisk();
}

void ProviderManager::loadFromDisk()
{
    // 1) 优先读 provider.txt
    fs::path p = providerStateFile();
    std::error_code ec;
    if (fs::exists(p, ec)) {
        std::ifstream ifs(p);
        std::string s;
        std::getline(ifs, s);
        // trim
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        if (!s.empty()) {
            current_.store(providerKindFromString(s));
            XAI_LOG_INFO("ProviderManager: loaded provider='{}' from {}", s, p.string());
            return;
        }
    }
    // 2) fallback：config.json 的 provider 字段
    const auto& cfg = Config::instance().get();
    current_.store(providerKindFromString(cfg.provider));
    XAI_LOG_INFO("ProviderManager: loaded provider='{}' from config.json", cfg.provider);
}

void ProviderManager::saveToDisk(ProviderKind k)
{
    fs::path p = providerStateFile();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p, std::ios::trunc);
    if (!ofs) {
        XAI_LOG_WARN("ProviderManager: cannot write {}", p.string());
        return;
    }
    ofs << providerKindToString(k) << "\n";
}

IChatProvider* ProviderManager::current()
{
    initIfNeeded();
    return get(current_.load());
}

ProviderKind ProviderManager::currentKind()
{
    initIfNeeded();
    return current_.load();
}

void ProviderManager::setProvider(ProviderKind kind)
{
    initIfNeeded();
    if (current_.load() == kind) return;
    current_.store(kind);
    saveToDisk(kind);
    XAI_LOG_INFO("ProviderManager: switched to {}", providerKindToString(kind));
}

}  // namespace x64ai
