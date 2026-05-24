// ai/copilot_chat_client.h
//
// GitHub Copilot Chat API 客户端。
//   - 端点：https://api.githubcopilot.com/chat/completions
//   - 必带 header: Authorization, Editor-Version, Editor-Plugin-Version,
//                  Copilot-Integration-Id, OpenAI-Intent
//   - 支持 SSE 流式与非流式
//
// M3.3 起作为 IChatProvider 的一种实现，可由 ProviderManager 切换。
#pragma once

#include "ai/chat_provider.h"

namespace x64ai {

class CopilotChatClient : public IChatProvider {
public:
    static CopilotChatClient& instance();

    // === IChatProvider ===
    ProviderKind kind() const override { return ProviderKind::Copilot; }
    std::string  displayName() const override { return "GitHub Copilot"; }

    bool                     isAuthenticated(std::string* outReason = nullptr) const override;
    std::vector<std::string> listModels() override;
    void                     streamChat(const ChatRequest& req,
                                        const ChatStreamCallbacks& cb) override;
    std::string              defaultModel() const override;  // 取自 config.defaultModel

private:
    CopilotChatClient() = default;
};

}  // namespace x64ai
