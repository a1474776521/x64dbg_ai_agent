// storage/session_store.cpp
#include "storage/session_store.h"

#include <cstring>
#include <ctime>

#include <sqlite3.h>
// 注意：sqlite-vec.h 默认会包含 sqlite3ext.h，把所有 sqlite3_* 宏化为
// sqlite3_api 间接调用；我们是 host 进程而非 sqlite 扩展 dll，因此必须先
// 定义 SQLITE_CORE 让它走直接调用路径。
#ifndef SQLITE_CORE
#  define SQLITE_CORE
#endif
#include <sqlite-vec.h>

#include "storage/meta_keys.h"
#include "util/encoding.h"
#include "util/logging.h"
#include "util/paths.h"

namespace x64ai {

namespace {

std::int64_t nowEpoch() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

// 在 sqlite3_open 之前注册 vec 扩展为自动加载，确保新连接都带上。
// sqlite3_auto_extension 是进程级状态，重复注册无害（sqlite 内部去重）。
void ensureVecAutoExtension() {
    static bool registered = false;
    if (registered) return;
    int rc = sqlite3_auto_extension(reinterpret_cast<void(*)()>(sqlite3_vec_init));
    if (rc != SQLITE_OK) {
        XAI_LOG_ERROR("sqlite3_auto_extension(sqlite3_vec_init) rc={}", rc);
    }
    registered = true;
}

bool execOrLog(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        XAI_LOG_ERROR("sqlite exec failed rc={} sql={} err={}",
                      rc, sql, err ? err : "");
        sqlite3_free(err);
        return false;
    }
    return true;
}

}  // namespace

SessionStore::SessionStore(const std::string& projectSha256Hex, int embeddingDim)
    : projectId_(projectSha256Hex), embDim_(embeddingDim)
{
    ensureVecAutoExtension();
    if (!open() || !initSchema()) {
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return;
    }
    // S3-G4：旧库 meta 字段（exe_path/exe_filename）2026-05-25 之前可能
    // 以 ACP（GBK）写入。检测非法 UTF-8 即按 ACP 重解码后回写为 UTF-8，
    // 确保 UI / 历史浏览器读出来不再乱码。
    // 该迁移幂等：合法 UTF-8 会跳过；只在首次打开旧库时触发一次。
    if (db_) {
        for (const char* key : { meta_keys::kExePath, meta_keys::kExeFilename }) {
            auto v = getMeta(key);
            if (!v || v->empty()) continue;
            if (isValidUtf8(*v)) continue;
            std::string fixed = ansiToUtf8(*v);
            if (fixed.empty() || !isValidUtf8(fixed)) {
                XAI_LOG_WARN("meta migrate skip key={} (ACP decode failed)", key);
                continue;
            }
            setMeta(key, fixed);
            XAI_LOG_INFO("meta migrate ACP->UTF-8 key={} old_bytes={} new={}",
                         key, v->size(), fixed);
        }
    }
}

SessionStore::~SessionStore() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool SessionStore::open() {
    // sqlite3_open 接 UTF-8 路径（SQLite 官方约定）。
    // 关键：path.string() 在 MSVC 走 ACP，中文路径会乱码 → 必须 fsPathToUtf8。
    auto fp   = projectDbPath(projectId_);
    auto path = fsPathToUtf8(fp);
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        XAI_LOG_ERROR("sqlite_open failed rc={} path={}", rc, path);
        return false;
    }
    XAI_LOG_INFO("sqlite opened: {}", path);
    // WAL + 外键
    execOrLog(db_, "PRAGMA journal_mode=WAL;");
    execOrLog(db_, "PRAGMA foreign_keys=ON;");
    execOrLog(db_, "PRAGMA synchronous=NORMAL;");
    return true;
}

bool SessionStore::initSchema() {
    if (!db_) return false;

    static const char* kSchema =
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  title TEXT NOT NULL,"
        "  model TEXT NOT NULL DEFAULT '',"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL"
        ");"

        "CREATE TABLE IF NOT EXISTS messages ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
        "  role TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL,"
        // K-41a: function calling 续跑用三列（详见 MessageRow 注释）
        "  tool_call_id TEXT NOT NULL DEFAULT '',"
        "  tool_name TEXT NOT NULL DEFAULT '',"
        "  tool_calls TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id);"

        "CREATE TABLE IF NOT EXISTS chunks ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  kind TEXT NOT NULL,"
        "  va INTEGER NOT NULL DEFAULT 0,"
        "  text TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_chunks_va ON chunks(va);"

        // 项目元信息（K-V）：exe_path / exe_filename / first_seen / last_seen 等。
        // 设计为通用 K-V 表，避免后续每加一个字段就 ALTER TABLE。
        "CREATE TABLE IF NOT EXISTS meta ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL DEFAULT ''"
        ");";

    if (!execOrLog(db_, kSchema)) return false;

    // sqlite-vec 虚拟表（rowid = chunks.id），维度由 embDim_ 决定
    char vecSql[256];
    std::snprintf(vecSql, sizeof(vecSql),
                  "CREATE VIRTUAL TABLE IF NOT EXISTS vec_chunks USING vec0("
                  "  embedding float[%d]"
                  ");",
                  embDim_);
    if (!execOrLog(db_, vecSql)) {
        XAI_LOG_ERROR("create vec_chunks failed (sqlite-vec not loaded?)");
        return false;
    }

    // K-41a: 检测老库 messages 表是否缺新列（tool_call_id / tool_name / tool_calls）。
    // A 方案（删整库）：不自动 migration，仅打 WARN 提示用户手动删除 .db 重启。
    // 检测到旧 schema 后 appendMessageEx / listMessages 触及新列时 sqlite 会直接返回错误，
    // function calling agent 续跑会无法工作（普通对话仍能跑）。
    {
        sqlite3_stmt* st = nullptr;
        bool hasToolCallId = false, hasToolName = false, hasToolCalls = false;
        if (sqlite3_prepare_v2(db_, "PRAGMA table_info(messages);",
                               -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const auto* col = sqlite3_column_text(st, 1);
                if (!col) continue;
                std::string name = reinterpret_cast<const char*>(col);
                if (name == "tool_call_id") hasToolCallId = true;
                else if (name == "tool_name") hasToolName = true;
                else if (name == "tool_calls") hasToolCalls = true;
            }
            sqlite3_finalize(st);
        }
        if (!hasToolCallId || !hasToolName || !hasToolCalls) {
            XAI_LOG_WARN("SessionStore: messages table missing K-41a columns "
                         "(tool_call_id={} tool_name={} tool_calls={}). "
                         "Old DB detected. To enable agent multi-round continuation "
                         "(\"continue +10 rounds\" button), please close x64dbg and "
                         "delete %APPDATA%\\x64dbg-ai-plugin\\projects\\{}.db, "
                         "then restart.",
                         hasToolCallId, hasToolName, hasToolCalls, projectId_);
        }
    }
    return true;
}

// ---- sessions ----

int64_t SessionStore::createSession(const std::string& title, const std::string& model) {
    if (!db_) return 0;
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO sessions(title,model,created_at,updated_at) VALUES(?,?,?,?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        XAI_LOG_ERROR("prepare createSession failed: {}", sqlite3_errmsg(db_));
        return 0;
    }
    auto t = nowEpoch();
    sqlite3_bind_text (st, 1, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, t);
    sqlite3_bind_int64(st, 4, t);

    int64_t id = 0;
    if (sqlite3_step(st) == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(db_);
    } else {
        XAI_LOG_ERROR("createSession step failed: {}", sqlite3_errmsg(db_));
    }
    sqlite3_finalize(st);
    return id;
}

std::vector<SessionRow> SessionStore::listSessions() {
    std::vector<SessionRow> out;
    if (!db_) return out;
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT id,title,model,created_at,updated_at FROM sessions "
        "ORDER BY updated_at DESC;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;

    while (sqlite3_step(st) == SQLITE_ROW) {
        SessionRow r;
        r.id        = sqlite3_column_int64(st, 0);
        const auto* t = sqlite3_column_text(st, 1);
        const auto* m = sqlite3_column_text(st, 2);
        r.title     = t ? reinterpret_cast<const char*>(t) : "";
        r.model     = m ? reinterpret_cast<const char*>(m) : "";
        r.createdAt = sqlite3_column_int64(st, 3);
        r.updatedAt = sqlite3_column_int64(st, 4);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return out;
}

bool SessionStore::renameSession(int64_t id, const std::string& title) {
    if (!db_) return false;
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE sessions SET title=?, updated_at=? WHERE id=?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text (st, 1, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, nowEpoch());
    sqlite3_bind_int64(st, 3, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool SessionStore::updateSessionModel(int64_t id, const std::string& model) {
    if (!db_) return false;
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE sessions SET model=?, updated_at=? WHERE id=?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text (st, 1, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, nowEpoch());
    sqlite3_bind_int64(st, 3, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool SessionStore::deleteSession(int64_t id) {
    if (!db_) return false;
    std::lock_guard<std::mutex> lk(mtx_);

    // 显式删 messages（也启用了 FK CASCADE，但双保险）
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM messages WHERE session_id=?;", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE id=?;", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

// ---- messages ----

int64_t SessionStore::appendMessage(int64_t sessionId,
                                    const std::string& role,
                                    const std::string& content) {
    // 老 3 参数接口：扩展字段全空委托给 appendMessageEx
    return appendMessageEx(sessionId, role, content,
                           /*toolCallId*/ "", /*toolName*/ "", /*toolCalls*/ "");
}

int64_t SessionStore::appendMessageEx(int64_t sessionId,
                                      const std::string& role,
                                      const std::string& content,
                                      const std::string& toolCallId,
                                      const std::string& toolName,
                                      const std::string& toolCalls) {
    if (!db_) return 0;
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* st = nullptr;
    // K-41a: 新增 tool_call_id / tool_name / tool_calls 三列
    const char* sql =
        "INSERT INTO messages(session_id,role,content,created_at,"
        "tool_call_id,tool_name,tool_calls) VALUES(?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        XAI_LOG_ERROR("appendMessageEx prepare failed: {} "
                      "(old schema? see SessionStore::initSchema WARN)",
                      sqlite3_errmsg(db_));
        return 0;
    }
    auto t = nowEpoch();
    sqlite3_bind_int64(st, 1, sessionId);
    sqlite3_bind_text (st, 2, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 3, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, t);
    sqlite3_bind_text (st, 5, toolCallId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 6, toolName.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 7, toolCalls.c_str(),  -1, SQLITE_TRANSIENT);

    int64_t id = 0;
    if (sqlite3_step(st) == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(db_);
    }
    sqlite3_finalize(st);

    // touch session.updated_at
    sqlite3_prepare_v2(db_, "UPDATE sessions SET updated_at=? WHERE id=?;",
                      -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, t);
    sqlite3_bind_int64(st, 2, sessionId);
    sqlite3_step(st);
    sqlite3_finalize(st);

    return id;
}

std::vector<MessageRow> SessionStore::listMessages(int64_t sessionId) {
    std::vector<MessageRow> out;
    if (!db_) return out;
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* st = nullptr;
    // K-41a: 多读 tool_call_id / tool_name / tool_calls 三列；老库（缺列）prepare 会失败
    const char* sql =
        "SELECT id,session_id,role,content,created_at,"
        "tool_call_id,tool_name,tool_calls FROM messages "
        "WHERE session_id=? ORDER BY id ASC;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        XAI_LOG_WARN("listMessages prepare failed: {} "
                     "(old schema? see SessionStore::initSchema WARN)",
                     sqlite3_errmsg(db_));
        return out;
    }
    sqlite3_bind_int64(st, 1, sessionId);

    while (sqlite3_step(st) == SQLITE_ROW) {
        MessageRow r;
        r.id         = sqlite3_column_int64(st, 0);
        r.sessionId  = sqlite3_column_int64(st, 1);
        const auto* role    = sqlite3_column_text(st, 2);
        const auto* content = sqlite3_column_text(st, 3);
        r.role       = role    ? reinterpret_cast<const char*>(role)    : "";
        r.content    = content ? reinterpret_cast<const char*>(content) : "";
        r.createdAt  = sqlite3_column_int64(st, 4);
        const auto* tci = sqlite3_column_text(st, 5);
        const auto* tn  = sqlite3_column_text(st, 6);
        const auto* tcs = sqlite3_column_text(st, 7);
        r.toolCallId = tci ? reinterpret_cast<const char*>(tci) : "";
        r.toolName   = tn  ? reinterpret_cast<const char*>(tn)  : "";
        r.toolCalls  = tcs ? reinterpret_cast<const char*>(tcs) : "";
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return out;
}

// ---- chunks ----

int64_t SessionStore::addChunk(const std::string& kind,
                               std::uint64_t va,
                               const std::string& text,
                               const std::vector<float>& embedding) {
    if (!db_) return 0;
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO chunks(kind,va,text,created_at) VALUES(?,?,?,?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text (st, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(va));
    sqlite3_bind_text (st, 3, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, nowEpoch());

    int64_t id = 0;
    if (sqlite3_step(st) == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(db_);
    } else {
        XAI_LOG_ERROR("addChunk insert failed: {}", sqlite3_errmsg(db_));
    }
    sqlite3_finalize(st);
    if (id == 0) return 0;

    // 写 vec 索引（仅当 embedding 维度匹配）
    if (static_cast<int>(embedding.size()) == embDim_) {
        sqlite3_stmt* vst = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO vec_chunks(rowid, embedding) VALUES(?,?);",
                -1, &vst, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(vst, 1, id);
            sqlite3_bind_blob (vst, 2, embedding.data(),
                               static_cast<int>(embedding.size() * sizeof(float)),
                               SQLITE_TRANSIENT);
            if (sqlite3_step(vst) != SQLITE_DONE) {
                XAI_LOG_ERROR("vec_chunks insert failed: {}", sqlite3_errmsg(db_));
            }
            sqlite3_finalize(vst);
        }
    } else if (!embedding.empty()) {
        XAI_LOG_ERROR("addChunk embedding dim mismatch: got {} expect {}",
                      embedding.size(), embDim_);
    }
    return id;
}

std::vector<RetrievedChunk> SessionStore::searchSimilar(
    const std::vector<float>& queryEmbedding, int topK)
{
    std::vector<RetrievedChunk> out;
    if (!db_ || static_cast<int>(queryEmbedding.size()) != embDim_ || topK <= 0) {
        return out;
    }
    std::lock_guard<std::mutex> lk(mtx_);

    sqlite3_stmt* st = nullptr;
    // sqlite-vec 的 KNN 查询语法
    const char* sql =
        "SELECT c.id, c.kind, c.va, c.text, c.created_at, v.distance "
        "FROM vec_chunks v JOIN chunks c ON c.id = v.rowid "
        "WHERE v.embedding MATCH ? AND k = ? "
        "ORDER BY v.distance;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        XAI_LOG_ERROR("prepare searchSimilar failed: {}", sqlite3_errmsg(db_));
        return out;
    }
    sqlite3_bind_blob(st, 1, queryEmbedding.data(),
                      static_cast<int>(queryEmbedding.size() * sizeof(float)),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 2, topK);

    while (sqlite3_step(st) == SQLITE_ROW) {
        RetrievedChunk r;
        r.chunk.id        = sqlite3_column_int64(st, 0);
        const auto* kind  = sqlite3_column_text(st, 1);
        r.chunk.kind      = kind ? reinterpret_cast<const char*>(kind) : "";
        r.chunk.va        = static_cast<std::uint64_t>(sqlite3_column_int64(st, 2));
        const auto* txt   = sqlite3_column_text(st, 3);
        r.chunk.text      = txt ? reinterpret_cast<const char*>(txt) : "";
        r.chunk.createdAt = sqlite3_column_int64(st, 4);
        r.distance        = static_cast<float>(sqlite3_column_double(st, 5));
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return out;
}

int64_t SessionStore::chunkCount() {
    if (!db_) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_stmt* st = nullptr;
    int64_t n = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM chunks;", -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

// ---- meta ----

bool SessionStore::setMeta(const std::string& key, const std::string& value) {
    if (!db_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_stmt* st = nullptr;
    // UPSERT：sqlite 3.24+ 支持 ON CONFLICT DO UPDATE
    const char* sql =
        "INSERT INTO meta(key,value) VALUES(?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        XAI_LOG_ERROR("setMeta prepare failed: {}", sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_text(st, 1, key.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool SessionStore::setMetaIfAbsent(const std::string& key, const std::string& value) {
    if (!db_) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT OR IGNORE INTO meta(key,value) VALUES(?,?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, key.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

std::optional<std::string> SessionStore::getMeta(const std::string& key) {
    if (!db_) return std::nullopt;
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_stmt* st = nullptr;
    std::optional<std::string> out;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM meta WHERE key=?;",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const auto* v = sqlite3_column_text(st, 0);
            out = v ? std::string(reinterpret_cast<const char*>(v)) : std::string();
        }
    }
    sqlite3_finalize(st);
    return out;
}

}  // namespace x64ai
