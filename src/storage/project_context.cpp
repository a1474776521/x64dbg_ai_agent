// storage/project_context.cpp
#include "storage/project_context.h"

#include <ctime>
#include <filesystem>
#include <thread>

#include "_plugins.h"
#include "_scriptapi_module.h"
#include "bridgemain.h"

#include "dbg/event_bus.h"
#include "storage/meta_keys.h"
#include "util/encoding.h"
#include "util/hashing.h"
#include "util/logging.h"

namespace x64ai {

ProjectContext& ProjectContext::instance() {
    static ProjectContext g;
    return g;
}

void ProjectContext::onDebugStart(const std::string& filePath) {
    // 入参 filePath 约定为 UTF-8（cbInitDebug 已在边界完成 ANSI->UTF-8）。
    // 兜底：如果上层漏转码且包含非法 UTF-8 字节，则按 ACP 再补一次。
    std::string path = filePath;
    if (!path.empty() && !isValidUtf8(path)) {
        XAI_LOG_WARN("ProjectContext::onDebugStart: filePath not valid UTF-8, fallback ansiToUtf8");
        path = ansiToUtf8(filePath);
    }
    if (path.empty()) {
        char buf[MAX_PATH] = {};
        if (Script::Module::GetMainModulePath(buf)) {
            // x64dbg SDK char* 即 UTF-8；直接使用，必要时下方 isValidUtf8 兜底。
            path = buf;
            if (!path.empty() && !isValidUtf8(path)) {
                XAI_LOG_WARN("GetMainModulePath returned non-UTF-8 bytes, fallback ansiToUtf8");
                path = ansiToUtf8(buf);
            }
        }
    }
    if (path.empty()) {
        XAI_LOG_ERROR("ProjectContext::onDebugStart: cannot resolve main module path");
        return;
    }

    // 主线程同步部分：抢占 generation、记录 path、清空旧 store（防止读到老的）。
    const uint64_t myGen = ++generation_;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        mainModulePath_ = path;  // UTF-8
        projectId_.clear();
        store_.reset();
    }
    indexing_.store(true);
    XAI_LOG_INFO("project indexing started: gen={} path={}", myGen, path);

    // 后台线程：SHA256 + sqlite 打开 + meta 写。完成后比较 generation 决定是否安装。
    std::thread([this, path, myGen]() {
        std::error_code ec;
        // 关键：fs::path 必须用 UTF-8 -> wstring 构造，否则 MSVC 按 ACP 解码中文路径会乱码。
        auto fp = fsPathFromUtf8(path);
        if (!std::filesystem::exists(fp, ec)) {
            XAI_LOG_ERROR("main module path not exist: {}", path);
            if (myGen == generation_.load()) indexing_.store(false);
            return;
        }

        auto sha = sha256File(fp);
        if (sha.empty()) {
            XAI_LOG_ERROR("sha256 failed for {}", path);
            if (myGen == generation_.load()) indexing_.store(false);
            return;
        }

        // 早退：被新一轮 onDebugStart 抢占了。
        if (myGen != generation_.load()) {
            XAI_LOG_INFO("project indexing dropped (preempted): gen={} sha={}", myGen, sha);
            return;
        }

        auto store = std::make_shared<SessionStore>(sha);
        if (store && store->isOpen()) {
            auto epoch = std::to_string(static_cast<long long>(std::time(nullptr)));
            store->setMeta(meta_keys::kExePath,     path);                        // UTF-8
            store->setMeta(meta_keys::kExeFilename, fsPathToUtf8(fp.filename())); // UTF-8
            store->setMetaIfAbsent(meta_keys::kFirstSeen, epoch);
            store->setMeta(meta_keys::kLastSeen,    epoch);
        }

        // 二次检查 generation（meta 写期间也可能被抢占）。
        if (myGen != generation_.load()) {
            XAI_LOG_INFO("project indexing dropped after meta write: gen={} sha={}", myGen, sha);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            store_     = std::move(store);
            projectId_ = sha;
        }
        indexing_.store(false);
        XAI_LOG_INFO("project opened: gen={} sha256={} path={}", myGen, sha, path);

        // S3：广播 store 已就绪；UI 侧（AssistantPanel/session_list）订阅后刷新状态栏与会话列表。
        // payload.raw 指向 sha 的栈拷贝，handler 必须同步消费完（publish 同步触发）。
        DbgEventPayload p{};
        p.raw = const_cast<std::string*>(&sha);
        EventBus::instance().publish(DbgEvent::ProjectStoreReady, p);
    }).detach();
}

void ProjectContext::onDebugStop() {
    // 抢占任何在途的后台 SHA256：generation 自增使其落盘判断失败。
    ++generation_;
    indexing_.store(false);
    std::lock_guard<std::mutex> lk(mtx_);
    store_.reset();
    projectId_.clear();
    mainModulePath_.clear();
    XAI_LOG_INFO("project closed");
}

std::shared_ptr<SessionStore> ProjectContext::store() {
    std::lock_guard<std::mutex> lk(mtx_);
    return store_;
}

std::string ProjectContext::projectId() {
    std::lock_guard<std::mutex> lk(mtx_);
    return projectId_;
}

std::string ProjectContext::mainModulePath() {
    std::lock_guard<std::mutex> lk(mtx_);
    return mainModulePath_;
}

}  // namespace x64ai
