// storage/session_store.h
//
// 按目标程序 SHA256 维护一个独立 sqlite 数据库，承载：
//   - sessions  : 多会话
//   - messages  : 每会话的消息历史
//   - chunks    : RAG 文本块（反汇编片段、字符串、注释等）
//   - vec_chunks: sqlite-vec 虚拟表，存 1536-d float embedding
//
// 所有公开方法均为线程安全（内部有 mutex），但每个 SessionStore 实例
// 绑定一个目标程序的 .db 文件。切换被调试程序时构造新实例。
#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace x64ai {

struct SessionRow {
    int64_t      id = 0;
    std::string  title;
    std::string  model;
    std::int64_t createdAt = 0;
    std::int64_t updatedAt = 0;
};

struct MessageRow {
    int64_t      id = 0;
    int64_t      sessionId = 0;
    std::string  role;       // "user" / "assistant" / "system" / "tool"
    std::string  content;
    std::int64_t createdAt = 0;
};

struct ChunkRow {
    int64_t      id = 0;
    std::string  kind;        // "asm" / "string" / "api" / "note" ...
    std::uint64_t va = 0;     // 关联虚拟地址（可 0）
    std::string  text;        // 原始文本
    std::int64_t createdAt = 0;
};

struct RetrievedChunk {
    ChunkRow chunk;
    float    distance = 0.0f;  // L2 距离，越小越相关
};

class SessionStore {
public:
    // 打开/创建对应 SHA256 的 .db；embeddingDim 默认 1536。
    SessionStore(const std::string& projectSha256Hex, int embeddingDim = 1536);
    ~SessionStore();

    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    bool isOpen() const { return db_ != nullptr; }
    const std::string& projectId() const { return projectId_; }
    int embeddingDim() const { return embDim_; }

    // ---- sessions ----
    int64_t                 createSession(const std::string& title, const std::string& model);
    std::vector<SessionRow> listSessions();
    bool                    renameSession(int64_t id, const std::string& title);
    bool                    updateSessionModel(int64_t id, const std::string& model);
    bool                    deleteSession(int64_t id);   // 同时删除其 messages

    // ---- messages ----
    int64_t                 appendMessage(int64_t sessionId,
                                          const std::string& role,
                                          const std::string& content);
    std::vector<MessageRow> listMessages(int64_t sessionId);

    // ---- chunks (RAG) ----
    // 写入一个文本块；若 embedding 非空则同步写入 vec_chunks。
    // embedding.size() 必须等于 embeddingDim()，否则只写文本不建索引。
    int64_t addChunk(const std::string& kind,
                     std::uint64_t va,
                     const std::string& text,
                     const std::vector<float>& embedding);

    // 用 query embedding 取 top-k 最近邻；返回的 chunks 已按距离升序。
    std::vector<RetrievedChunk> searchSimilar(const std::vector<float>& queryEmbedding,
                                              int topK);

    // 统计：用于 UI 展示
    int64_t chunkCount();

    // ---- meta (K-V 元信息表) ----
    // 写入/覆盖一对元信息；常用键见 meta_keys.h（exe_path/exe_filename/first_seen/last_seen）。
    bool                       setMeta(const std::string& key, const std::string& value);
    // 仅当 key 不存在时才插入；用于 first_seen 之类"首次记录"语义。
    bool                       setMetaIfAbsent(const std::string& key, const std::string& value);
    // 查询：不存在返回 std::nullopt。
    std::optional<std::string> getMeta(const std::string& key);

private:
    bool open();
    bool initSchema();

    std::string             projectId_;
    int                     embDim_ = 1536;
    sqlite3*                db_ = nullptr;
    mutable std::mutex      mtx_;
};

}  // namespace x64ai
