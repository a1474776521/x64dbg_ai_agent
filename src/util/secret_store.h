// util/secret_store.h
//
// Windows DPAPI 加密的小型密钥存储。每个 secret 存为
// %APPDATA%/x64dbg-ai-plugin/secrets/<name>.bin。
// 加密范围 = CRYPTPROTECT_LOCAL_MACHINE 不开（仅当前用户可解密）。
#pragma once

#include <optional>
#include <string>

namespace x64ai {

class SecretStore {
public:
    static SecretStore& instance();

    bool saveSecret(const std::string& name, const std::string& plaintext);
    std::optional<std::string> loadSecret(const std::string& name);
    bool deleteSecret(const std::string& name);

    // 解析"读取顺序"：先 DPAPI -> 再 config.json 兜底字段（明文，开发用）。
    // 找不到返回 nullopt。
    std::optional<std::string> resolveSecret(const std::string& name,
                                             const std::string& configFallback);

private:
    SecretStore() = default;
};

}  // namespace x64ai
