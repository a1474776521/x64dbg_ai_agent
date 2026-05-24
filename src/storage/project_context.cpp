// storage/project_context.cpp
#include "storage/project_context.h"

#include <ctime>
#include <filesystem>
#include <thread>

#include "_plugins.h"
#include "_scriptapi_module.h"
#include "bridgemain.h"

#include "storage/meta_keys.h"
#include "util/hashing.h"
#include "util/logging.h"

namespace x64ai {

ProjectContext& ProjectContext::instance() {
    static ProjectContext g;
    return g;
}

void ProjectContext::onDebugStart(const std::string& filePath) {
    std::string path = filePath;
    if (path.empty()) {
        char buf[MAX_PATH] = {};
        if (Script::Module::GetMainModulePath(buf)) {
            path = buf;
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
        mainModulePath_ = path;
        projectId_.clear();
        store_.reset();
    }
    indexing_.store(true);
    XAI_LOG_INFO("project indexing started: gen={} path={}", myGen, path);

    // 后台线程：SHA256 + sqlite 打开 + meta 写。完成后比较 generation 决定是否安装。
    std::thread([this, path, myGen]() {
        std::error_code ec;
        auto fp = std::filesystem::path(path);
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
            store->setMeta(meta_keys::kExePath,     path);
            store->setMeta(meta_keys::kExeFilename, fp.filename().string());
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
