// storage/project_context.h
//
// 把"当前被调试程序"映射到一个 SessionStore（按主模块路径 SHA256 分库）。
// x64dbg 的调试启停回调里调用 onDebugStart/onDebugStop 切换 store。
//
// S0-C2 (2026-05-24)：onDebugStart 内部异步化。原先同步 SHA256 + sqlite 打开
// 在调试线程阻塞，几百 MB exe 启动卡 2–6 秒。现改为：
//   1) 主线程立即抢占 generation_++，并清空当前 store_（projectId_ 留空）
//   2) detach 后台线程做 SHA256 + sqlite + meta 落盘
//   3) 后台线程完成后在自身 generation 仍等于当前 generation_ 时安装 store_
//      （否则被新一轮 onDebugStart 抢占了，老结果丢弃）
// 消费者读 store() 时若为 nullptr，应软返回（既有调用处已全部检查）。
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "storage/session_store.h"

namespace x64ai {

class ProjectContext {
public:
    static ProjectContext& instance();

    // x64dbg CB_INITDEBUG / CB_STOPDEBUG 触发；filePath 来自
    // Script::Module::GetMainModulePath（utf8）。立即返回；SHA256 在后台线程。
    void onDebugStart(const std::string& filePath = {});
    void onDebugStop();

    // 返回当前活动 store；未调试 / SHA256 尚未完成时返回 nullptr。
    std::shared_ptr<SessionStore> store();

    // 当前程序 SHA256（hex）；SHA 计算未完成时返回空串。
    std::string projectId();

    // 当前主模块完整路径（仅供显示）。SHA 计算未完成也可读，便于 UI 占位符。
    std::string mainModulePath();

    // 后台 SHA256 是否仍在进行中（UI 灰化 Agent 入口时用）。
    bool isIndexing() const { return indexing_.load(); }

private:
    ProjectContext() = default;

    std::mutex                     mtx_;
    std::shared_ptr<SessionStore>  store_;
    std::string                    projectId_;
    std::string                    mainModulePath_;

    // 每次 onDebugStart 自增；后台线程拿到自己的 generation，
    // 落盘前比较，若不一致说明已被新一轮抢占，丢弃结果。
    std::atomic<uint64_t> generation_{0};
    std::atomic<bool>     indexing_{false};
};

}  // namespace x64ai
