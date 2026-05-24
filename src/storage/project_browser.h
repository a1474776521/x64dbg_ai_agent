// storage/project_browser.h
//
// 跨 DB 只读浏览器：扫描 %APPDATA%/x64dbg-ai-plugin/projects/ 下所有
// .db 文件，按需打开任意一个旧库读 sessions/messages，用于"历史项目
// 会话浏览/导入"。
//
// 与 SessionStore 的关系：
//   - SessionStore 绑定 *当前* 项目的 SHA，会创建表、加载 sqlite-vec
//   - ProjectBrowser 直接用 sqlite3 原生 API 只读模式打开任意路径，
//     不触发 schema 变更、不依赖 vec 扩展，避免污染或冲突。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "storage/session_store.h"  // 复用 SessionRow / MessageRow

namespace x64ai {

struct ProjectDbInfo {
    std::string            sha256Hex;   // 文件名（去 .db 后缀）
    std::filesystem::path  path;
    std::uint64_t          sizeBytes = 0;
    std::int64_t           mtimeEpoch = 0;
    std::int64_t           sessionCount = 0;  // 失败时为 -1

    // K-01：meta 表读出的元信息；老库未写入时留空，UI 端需要 fallback 到 SHA 显示。
    std::string            exePath;       // 完整 EXE 路径（写入时刻）
    std::string            exeFilename;   // 仅文件名，便于列表展示
    std::int64_t           firstSeenEpoch = 0;
    std::int64_t           lastSeenEpoch  = 0;
};

class ProjectBrowser {
public:
    // 扫描 projects/*.db；按 mtime 降序；不打开 db 也能拿到 size/mtime；
    // 但 meta 字段（exePath/exeFilename/...）需要打开 db 才能读，本函数会
    // 顺手 ro 打开每个 db 读 meta 表（容错：表不存在/老库时留空）。
    // sessionCount 字段保留 -1（仍按需懒加载）。
    static std::vector<ProjectDbInfo> listProjectDbs();

    // 只读打开一个 .db，返回其 sessions（按 updated_at DESC）。
    // 失败返回空。
    static std::vector<SessionRow> listSessions(const std::filesystem::path& dbPath);

    // 只读读取指定 session 的全部 messages（按 id ASC）。
    static std::vector<MessageRow> listMessages(const std::filesystem::path& dbPath,
                                                std::int64_t sessionId);

    // K-02：清理空 / 老旧 db 文件。
    //   - 文件大小 == 0 的总是删（这种是 sqlite_open 后未建表/崩溃残留）
    //   - 大小 > 0 但 sessions 与 chunks 两表都为空且 mtime 早于 now - minAgeDays 的删
    //   - 同时尝试删 sibling 的 -wal / -shm / -journal 副本
    // 返回被删除的 db 文件数量（不含 sidecar）。
    // 注意：永不删除当前活动项目的 db；调用方需传入 activeSha 排除（可空）。
    static int cleanupEmptyDbs(int minAgeDays = 7,
                               const std::string& activeShaHex = {});
};

}  // namespace x64ai
