// storage/project_browser.cpp
#include "storage/project_browser.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <system_error>

#include <sqlite3.h>

#include "storage/meta_keys.h"
#include "util/encoding.h"
#include "util/logging.h"
#include "util/paths.h"

namespace x64ai {

namespace fs = std::filesystem;

namespace {

std::int64_t toEpochSeconds(const fs::file_time_type& ft) {
    // C++17：先转 system_clock 再 to_time_t；不同实现可能有 epoch 差异，
    // 这里用 cast 折中（精度到秒已足够展示）。
    using namespace std::chrono;
    auto sctp = time_point_cast<system_clock::duration>(
        ft - fs::file_time_type::clock::now() + system_clock::now());
    return static_cast<std::int64_t>(system_clock::to_time_t(sctp));
}

// 只读打开 db；URI 模式 mode=ro 避免任何写。WAL 文件已被原 owner 关闭，
// SQLite 在只读打开时不会回放 WAL（需 SQLITE_OPEN_NOMUTEX 也不写），
// 但仍能读到已 checkpoint 的数据；旧库通常已无 WAL 残留。
sqlite3* openReadOnly(const fs::path& path) {
    sqlite3* db = nullptr;
    // sqlite URI：用 file: 前缀；Windows 路径需要把反斜杠改成正斜杠。
    // 关键：generic_string() 走 ACP，中文路径会乱码 → 用 fsPathToUtf8(generic) 走 UTF-8。
    std::string uri = "file:";
    // 先取 wstring 把反斜杠替换为正斜杠，再转 UTF-8
    std::wstring ws = path.wstring();
    for (auto& c : ws) { if (c == L'\\') c = L'/'; }
    std::string s = wideToUtf8(ws);
    // URI 中需要把 ' '%20 等 escape，但常规 hex 文件名不含特殊字符；
    // 中文目录段若存在，sqlite3 URI 解析按 UTF-8 处理，已在 fsPathToUtf8 中正确编码。
    uri.append(s);
    uri.append("?mode=ro&immutable=1");
    int rc = sqlite3_open_v2(uri.c_str(), &db,
                             SQLITE_OPEN_READONLY | SQLITE_OPEN_URI,
                             nullptr);
    if (rc != SQLITE_OK) {
        XAI_LOG_ERROR("ProjectBrowser open ro failed rc={} path={}", rc, s);
        if (db) sqlite3_close(db);
        return nullptr;
    }
    return db;
}

// 在已打开的 db 上读一个 meta key；表不存在 / key 不存在均返回空串。
// 内部已 swallow 所有错误（老库没 meta 表是预期场景）。
std::string readMeta(sqlite3* db, const char* key) {
    if (!db) return {};
    sqlite3_stmt* st = nullptr;
    std::string out;
    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key=?;",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const auto* v = sqlite3_column_text(st, 0);
            if (v) out.assign(reinterpret_cast<const char*>(v));
        }
        sqlite3_finalize(st);
    }
    return out;
}

// 在已打开的 db 上数一张表的行数；表不存在返回 -1（区别于真实 0）。
std::int64_t countTable(sqlite3* db, const char* table) {
    if (!db) return -1;
    std::string sql = "SELECT COUNT(*) FROM ";
    sql += table;
    sql += ";";
    sqlite3_stmt* st = nullptr;
    std::int64_t n = -1;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            n = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    return n;
}

}  // namespace

std::vector<ProjectDbInfo> ProjectBrowser::listProjectDbs() {
    std::vector<ProjectDbInfo> out;
    auto dir = pluginProjectsDir();
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        return out;
    }
    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != fs::directory_iterator();
         it.increment(ec))
    {
        const auto& p = it->path();
        if (!it->is_regular_file(ec)) continue;
        if (p.extension() != ".db") continue;

        ProjectDbInfo info;
        info.sha256Hex = p.stem().string();  // sha256 是 ASCII，安全
        info.path      = p;
        std::error_code ec2;
        info.sizeBytes = static_cast<std::uint64_t>(fs::file_size(p, ec2));
        auto mt = fs::last_write_time(p, ec2);
        info.mtimeEpoch = ec2 ? 0 : toEpochSeconds(mt);
        info.sessionCount = -1;  // lazy

        // 顺手读 meta（开/关 db 的开销极小，几 ms 级；与列表展示同步走完）。
        // 0 字节文件直接跳过 open（sqlite 会失败且产生噪声日志）。
        if (info.sizeBytes > 0) {
            if (sqlite3* db = openReadOnly(p)) {
                info.exePath        = readMeta(db, meta_keys::kExePath);
                info.exeFilename    = readMeta(db, meta_keys::kExeFilename);
                auto firstStr       = readMeta(db, meta_keys::kFirstSeen);
                auto lastStr        = readMeta(db, meta_keys::kLastSeen);
                try { if (!firstStr.empty()) info.firstSeenEpoch = std::stoll(firstStr); } catch (...) {}
                try { if (!lastStr.empty())  info.lastSeenEpoch  = std::stoll(lastStr);  } catch (...) {}
                sqlite3_close(db);
            }
        }

        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(),
              [](const ProjectDbInfo& a, const ProjectDbInfo& b) {
                  return a.mtimeEpoch > b.mtimeEpoch;
              });
    return out;
}

std::vector<SessionRow> ProjectBrowser::listSessions(const fs::path& dbPath) {
    std::vector<SessionRow> out;
    sqlite3* db = openReadOnly(dbPath);
    if (!db) return out;

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT id,title,model,created_at,updated_at FROM sessions "
        "ORDER BY updated_at DESC;";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
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
    } else {
        XAI_LOG_WARN("ProjectBrowser listSessions prepare failed: {}",
                     sqlite3_errmsg(db));
    }
    sqlite3_close(db);
    return out;
}

std::vector<MessageRow> ProjectBrowser::listMessages(const fs::path& dbPath,
                                                    std::int64_t sessionId) {
    std::vector<MessageRow> out;
    sqlite3* db = openReadOnly(dbPath);
    if (!db) return out;

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT id,session_id,role,content,created_at FROM messages "
        "WHERE session_id=? ORDER BY id ASC;";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
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
            out.push_back(std::move(r));
        }
        sqlite3_finalize(st);
    } else {
        XAI_LOG_WARN("ProjectBrowser listMessages prepare failed: {}",
                     sqlite3_errmsg(db));
    }
    sqlite3_close(db);
    return out;
}

int ProjectBrowser::cleanupEmptyDbs(int minAgeDays, const std::string& activeShaHex) {
    int deleted = 0;
    auto dir = pluginProjectsDir();
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return 0;

    const auto nowSec = static_cast<std::int64_t>(std::time(nullptr));
    const std::int64_t minAgeSec = static_cast<std::int64_t>(minAgeDays) * 86400;

    // 先收集候选，避免边迭代边删
    std::vector<fs::path> toDelete;

    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != fs::directory_iterator();
         it.increment(ec))
    {
        const auto& p = it->path();
        std::error_code ec2;
        if (!it->is_regular_file(ec2)) continue;
        if (p.extension() != ".db") continue;

        std::string sha = p.stem().string();
        if (!activeShaHex.empty() && sha == activeShaHex) continue;  // 绝不动当前活动 db

        auto sz = fs::file_size(p, ec2);
        if (ec2) continue;

        // 规则 1：0 字节文件（sqlite_open 创建后未写任何内容）→ 直接删
        if (sz == 0) {
            toDelete.push_back(p);
            continue;
        }

        // 规则 2：sessions 与 chunks 均为空，且 mtime 早于 now - minAgeDays
        auto mt = fs::last_write_time(p, ec2);
        if (ec2) continue;
        auto mtimeSec = toEpochSeconds(mt);
        if (nowSec - mtimeSec < minAgeSec) continue;  // 太新，留着

        if (sqlite3* db = openReadOnly(p)) {
            auto sessions = countTable(db, "sessions");
            auto chunks   = countTable(db, "chunks");
            sqlite3_close(db);
            // 表不存在按 -1 处理；老库可能缺 chunks/vec_chunks，这里宽容：
            // 只要 sessions <= 0（含 -1）且 chunks <= 0（含 -1），并且文件已老，就清理
            if (sessions <= 0 && chunks <= 0) {
                toDelete.push_back(p);
            }
        }
        // db 打不开（损坏）也保留，让用户自己判断，不擅自删
    }

    // 执行删除（包含 sidecar -wal / -shm / -journal）
    for (const auto& p : toDelete) {
        std::error_code de;
        bool ok = fs::remove(p, de);
        if (!ok || de) {
            XAI_LOG_WARN("cleanup: failed to remove {} ec={}", fsPathToUtf8(p), de.message());
            continue;
        }
        ++deleted;
        XAI_LOG_INFO("cleanup: removed empty/stale db {}", fsPathToUtf8(p));

        for (const char* suf : {"-wal", "-shm", "-journal"}) {
            fs::path sidecar = p;
            sidecar += suf;
            std::error_code se;
            if (fs::exists(sidecar, se)) {
                fs::remove(sidecar, se);
            }
        }
    }
    return deleted;
}

}  // namespace x64ai
