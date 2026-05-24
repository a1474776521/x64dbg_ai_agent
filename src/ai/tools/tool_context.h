// ai/tools/tool_context.h
//
// ToolContext：传给 ITool::invoke，封装"工具运行时能用到的全局状态"。
//
// 当前所有工具都在 x64dbg 主进程内运行（同地址空间），所以这里只是个
// 收纳柜：让我们以后能 mock、能加权限控制、能记录每次 tool 访问。
//
// 不直接暴露 SessionStore / ProjectContext 全 API，工具只取自己要的那点。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace x64ai {

// 调用方（一般是 AgentLoop）填好后传给工具
struct ToolContext {
    // 当前活动 session 的指针（rag_search 需要）。可为空。
    void* sessionStore = nullptr;  // 真正类型 SessionStore*；工具按需 reinterpret
    std::int64_t sessionId = 0;

    // 当前调试目标的 SHA256（hex，可空）
    std::string targetSha;

    // 调试器是否在运行（CMDPAUSED/CMDRUNNING）
    // 为 false 时 read_memory / get_registers / get_callstack 等动态工具应直接拒绝
    bool debuggerActive = false;

    // S2-D：AgentWorker 的 cancel 标志。工具内有阻塞等待时（如 wait_for_event）
    // 应周期性轮询此标志并尽快返回。可为 nullptr（旧调用路径）。
    // 不接管所有权——指针生命周期由 AgentWorker 的 shared_ptr<atomic<bool>> 保证。
    const std::atomic<bool>* cancelFlag = nullptr;

    // 调用者可选：写一条 audit 日志（plugin.log），失败/截断时附加上下文。
    // 工具内部一般不直接用，由 ToolRegistry::dispatch 统一记录。
};

}  // namespace x64ai
