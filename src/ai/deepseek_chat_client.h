// ai/deepseek_chat_client.h
//
// DeepSeek 官方 API 客户端，OpenAI 兼容格式。
//   - 端点：https://api.deepseek.com/v1
//   - 模型：deepseek-chat / deepseek-reasoner（动态拉取）
//   - 鉴权：Authorization: Bearer sk-xxx；Key 走 SecretStore (DPAPI)
//
// 不需要 GitHub 那一套 Editor-* 头。
#pragma once

#include "ai/chat_provider.h"

#include <optional>
#include <string>

namespace x64ai {

class DeepSeekChatClient : public IChatProvider {
public:
    static DeepSeekChatClient& instance();

    // === IChatProvider ===
    ProviderKind kind() const override { return ProviderKind::DeepSeek; }
    std::string  displayName() const override { return "DeepSeek"; }

    bool                     isAuthenticated(std::string* outReason = nullptr) const override;
    std::vector<std::string> listModels() override;
    void                     streamChat(const ChatRequest& req,
                                        const ChatStreamCallbacks& cb) override;
    std::string              defaultModel() const override { return "deepseek-chat"; }

    // === Key 管理 ===
    // 把 sk-xxx 加密落盘（DPAPI）
    bool        saveApiKey(const std::string& key);
    // 删除磁盘上的 key
    bool        clearApiKey();
    // 读取已保存的 key（DPAPI）；返回空表示未设置
    std::optional<std::string> loadApiKey() const;
    // 返回 sk-xxxx****（前 8 后 4），未设置返回空
    std::string maskedApiKey() const;

private:
    DeepSeekChatClient() = default;
};

}  // namespace x64ai
