// ai/kspmas_chat_client.h
//
// 金山云 KSPmas API 客户端，OpenAI 兼容格式。
//   - 端点：https://kspmas.ksyun.com/v1
//   - 模型：deepseek-v4-pro（默认；其他模型由服务端 /models 决定或 preset 指定）
//   - 鉴权：Authorization: Bearer <api-key>；Key 走 SecretStore (DPAPI)
//
// 与 DeepSeek client 结构对称；差异：
//   1) 端点与默认模型不同
//   2) 不发 reasoning_content / 不回传 reasoning（KSPmas 不一定支持 thinking 模型）
//   3) defaultModel 改为 "deepseek-v4-pro"
//
// function calling：按"支持"处理；若 KSPmas 真不支持，会回 HTTP 400 立即可见。
#pragma once

#include "ai/chat_provider.h"

#include <optional>
#include <string>

namespace x64ai {

class KSPmasChatClient : public IChatProvider {
public:
    static KSPmasChatClient& instance();

    // === IChatProvider ===
    ProviderKind kind() const override { return ProviderKind::KSPmas; }
    std::string  displayName() const override { return "KSPmas"; }

    bool                     isAuthenticated(std::string* outReason = nullptr) const override;
    std::vector<std::string> listModels() override;
    void                     streamChat(const ChatRequest& req,
                                        const ChatStreamCallbacks& cb) override;
    std::string              defaultModel() const override { return "deepseek-v4-pro"; }

    // === Key 管理 ===
    // 把 key 加密落盘（DPAPI）
    bool        saveApiKey(const std::string& key);
    // 删除磁盘上的 key
    bool        clearApiKey();
    // 读取已保存的 key（DPAPI）；返回空表示未设置
    std::optional<std::string> loadApiKey() const;
    // 返回 xxxx****（前 8 后 4），未设置返回空
    std::string maskedApiKey() const;

private:
    KSPmasChatClient() = default;
};

}  // namespace x64ai
