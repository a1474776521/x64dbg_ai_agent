// ai/agent_loop.h
//
// AgentLoop：把 IChatProvider + ToolRegistry 编成多轮自主 agent。
//
// 模型协议：
//   1. user/system 消息送入 -> provider.streamChat
//   2. provider 通过 onDelta 增量回内容（透传给 UI）
//   3. provider 通过 onToolCalls 回吐一批 tool_calls（assistant message 完成）
//   4. AgentLoop 把 assistant 消息（含 tool_calls）追加到对话历史
//   5. 顺序执行每个 tool_call -> ToolRegistry::dispatch -> 结果作为 role=tool 消息追加
//   6. 回到 step 2 继续；直到不再有 tool_calls 或 maxIter 用尽
//
// 安全：
//   - 由调用方提供 ToolContext（含 sessionStore/debuggerActive）
//   - max_iter 默认 20，超限发 onMaxIter 让 UI 决定是否继续（再调一次 run）
//   - cancel 通过 std::atomic<bool>（调用方持有）；每轮入口 + tool dispatch 前后检查
//   - Copilot 自动降级：不传 tools 字段（其上游限制）
//
// 线程：streamChat 在调用线程同步阻塞，AgentLoop::run 本身也是阻塞的。
// 由 UI 端放后台线程。
#pragma once

#include "ai/chat_provider.h"
#include "ai/tools/tool_context.h"

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace x64ai {

struct AgentRunRequest {
    IChatProvider*           provider = nullptr;
    std::string              model;       // 空 -> provider->defaultModel()
    std::vector<ChatMessage> messages;    // 初始对话；执行后会原地追加 assistant + tool 消息
    double                   temperature  = 0.2;
    int                      maxTokens    = 0;
    int                      maxIter      = 20;

    // M4.6d：调用方按预设过滤后的工具白名单。
    // 为空 → AgentLoop 自动用 ToolRegistry::listChatTools()（全量）。
    std::vector<ChatTool>    tools;
};

struct AgentToolCallReport {
    std::string id;
    std::string name;
    std::string argumentsJson;
    std::string resultJson;     // 截断后的 serializeForLlm 结果
    bool        ok          = false;
    std::string error;
    long long   elapsedMs   = 0;
    bool        truncated   = false;
};

struct AgentRunCallbacks {
    // 内容流式回吐（与 ChatStreamCallbacks::onDelta 一致；轮次切换时不带分隔符）
    std::function<void(std::string_view delta)>          onAssistantDelta;
    // DeepSeek thinking 模型的推理过程流式回吐（可选；UI 可折叠显示）
    std::function<void(std::string_view delta)>          onAssistantReasoningDelta;
    // 一轮 assistant 消息收尾（content/toolCalls 都已最终）
    std::function<void(const ChatMessage& msg)>          onAssistantMessage;
    // 单个 tool_call 执行完成
    std::function<void(const AgentToolCallReport& rep)>  onToolReport;
    // 致命错误（provider 报错 / 取消 / 内部异常）
    std::function<void(const std::string& err)>          onError;
    // 达到 maxIter 但还有 tool_calls 没消化（UI 可以提供"继续"按钮）
    std::function<void(int iter, int pendingCalls)>      onMaxIterReached;
    // G-2 (2026-05-25): 每轮 LLM 调用的 token 用量 + cache 命中
    std::function<void(const UsageInfo&)>                onUsage;
    // 正常完成（最后一轮没 tool_calls）
    std::function<void()>                                onDone;
};

class AgentLoop {
public:
    // ctx 调用方填好（debuggerActive 等）；ctx 的 sessionStore 应来自 ProjectContext。
    // cancel 由调用方持有；任何时刻置 true 都会尽快返回（不抛异常）。
    // 返回值：本次实际完成的 iteration 数。
    static int run(AgentRunRequest&         req,
                   ToolContext&             ctx,
                   const AgentRunCallbacks& cb,
                   const std::atomic<bool>& cancel);
};

}  // namespace x64ai
