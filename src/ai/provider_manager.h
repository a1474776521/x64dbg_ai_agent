// ai/provider_manager.h
//
// ProviderManager：管理当前激活的 IChatProvider，并把选择持久化到
// %APPDATA%/x64dbg-ai-plugin/provider.txt。
//
// 用法：
//   auto* p = ProviderManager::instance().current();
//   p->streamChat(...);
#pragma once

#include "ai/chat_provider.h"

#include <atomic>
#include <mutex>

namespace x64ai {

class ProviderManager {
public:
    static ProviderManager& instance();

    // 当前 Provider；首次访问时按 provider.txt（或 config.provider）初始化。
    IChatProvider* current();

    // 当前 Provider 类型
    ProviderKind currentKind();

    // 切换；自动持久化到磁盘。空操作（同 kind）也允许。
    void setProvider(ProviderKind kind);

    // 通过单例获取任意一种 Provider（不修改 current）
    static IChatProvider* get(ProviderKind kind);

private:
    ProviderManager() = default;
    void initIfNeeded();
    void loadFromDisk();
    void saveToDisk(ProviderKind k);

    std::mutex                mu_;
    std::atomic<ProviderKind> current_{ProviderKind::Copilot};
    bool                      initialized_ = false;
};

}  // namespace x64ai
