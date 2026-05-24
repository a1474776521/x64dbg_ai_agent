// ai/embedding_client.h
//
// GitHub Models API 的 embedding 客户端。
//   - 端点：https://models.github.ai/inference/embeddings  （PAT Bearer）
//   - 模型：text-embedding-3-small（1536 维）
//   - 鉴权：从 SecretStore 取 "github_pat"，回退到 config.json 里的 github_pat
//
// 同步阻塞接口；调用方放后台线程。
#pragma once

#include <string>
#include <vector>

namespace x64ai {

class EmbeddingClient {
public:
    static EmbeddingClient& instance();

    // 单条文本 embedding。失败返回空 vector。
    std::vector<float> embed(const std::string& text);

    // 批量；失败时对应位置返回空 vector。
    std::vector<std::vector<float>> embedBatch(const std::vector<std::string>& texts);

    // 当前模型维度（默认 1536）
    int dim() const { return 1536; }

private:
    EmbeddingClient() = default;

    std::string resolveToken();
};

}  // namespace x64ai
