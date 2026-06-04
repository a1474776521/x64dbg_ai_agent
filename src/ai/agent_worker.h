// ai/agent_worker.h
//
// AgentWorker：把 AgentLoop（阻塞）封装成 Qt 友好的异步对象。
//
// 用法：
//   auto* w = new AgentWorker(parent);
//   connect(w, &AgentWorker::assistantDelta, view, [](QString d){ ... });
//   connect(w, &AgentWorker::toolCallStarted, view, ...);
//   ...
//   w->start(req);          // 内部 QtConcurrent::run，立刻返回
//   w->requestCancel();     // 协作取消
//
// 所有 signal 都是 QueuedConnection 安全的（emit 在 worker 线程；
// connect 默认 AutoConnection → 跨线程自动 QueuedConnection）。
//
// 生命周期：worker 自动持有 cancel/messages 副本；UI 端持有 w 即可。
// 完成（finished/failed/maxIterReached）发出后 worker 可被 deleteLater。
#pragma once

#include "ai/agent_loop.h"
#include "ai/agent_preset.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace x64ai {

class AgentWorker : public QObject {
    Q_OBJECT
public:
    explicit AgentWorker(QObject* parent = nullptr);
    ~AgentWorker() override;

    // 起一次 agent 跑（线程安全前提下只调一次；再调一次需另起 worker）。
    // req.provider 必填；req.messages 必须已经包含 user/system 初始消息。
    void start(AgentRunRequest req);

    // 协作取消；下一次 tool dispatch 或 round 入口检查。
    void requestCancel();

    bool isRunning() const { return running_.load(); }

    // K-41b: 取走 agent 跑完后内存里的最终对话（含 assistant.tool_calls + tool.toolCallId
    // 配对结构，可直接喂给下一次 AgentRunRequest.messages 实现续跑）。
    //
    // 调用时机：在 maxIterReached / finished signal 处理槽里调（此刻 worker 后台线程
    // 已经把 snapshot 填好，且 panel 在主线程读，由内部 mutex 保护跨线程访问）。
    // 调用后 snapshot 被 move 走，再次调返回空。
    //
    // 用途：K-41d 续跑按钮（"继续推理 +10 轮"）；其它路径无需关心。
    std::vector<ChatMessage> takeSnapshotMessages();

signals:
    // assistant 流式 token
    void assistantDelta(QString delta);
    // assistant 思考链流式 token（DeepSeek thinking 模型独有；其他模型从不发）
    void assistantReasoningDelta(QString delta);
    // 一轮 assistant 消息（含完整 content + toolCalls）收尾
    // toolCallNames 用于 UI 立刻新建对应卡片（pending 状态）
    // K-41b: 新增 toolCallsJson，OpenAI 标准 tool_calls JSON 数组字符串
    //        （形如 `[{"id":"call_x","type":"function","function":{"name":"...","arguments":"..."}}]`），
    //        无工具调用时为空串。用于 K-41c 持久化到 messages.tool_calls 列。
    void assistantMessage(QString content,
                          QStringList toolCallIds,
                          QStringList toolCallNames,
                          QString toolCallsJson);
    // 单个 tool_call 开始（实际 dispatch 前的瞬时通知；与 assistantMessage 顺序保证）
    void toolCallStarted(QString id, QString name, QString argsJson);
    // 单个 tool_call 完成
    void toolCallFinished(QString id,
                          QString name,
                          QString argsJson,
                          QString resultJson,
                          bool    ok,
                          QString error,
                          qint64  elapsedMs,
                          bool    truncated);
    // 致命错误 / 取消
    void failed(QString error);
    // 达到 maxIter
    void maxIterReached(int iter, int pendingCalls);
    // G-2 (2026-05-25): 每轮 LLM 调用结束后的 token 用量；hitRatio<0 表示 prompt_tokens=0
    void usageUpdated(int promptTokens,
                      int cachedPromptTokens,
                      int completionTokens,
                      int reasoningTokens,
                      double hitRatio);
    // 正常完成
    void finished(int iterations);

private:
    void runBlocking(AgentRunRequest req);

    std::atomic<bool>  running_{false};
    // cancel 用 shared_ptr 共享给后台 lambda，便于 worker 提前析构也安全
    std::shared_ptr<std::atomic<bool>> cancel_;

    // K-41b: agent 跑完后的最终对话副本（含 tool_calls 配对结构），
    // 后台线程在 onMaxIterReached / onDone 时填充，主线程通过
    // takeSnapshotMessages() 拿走，mutex 保护跨线程访问。
    mutable std::mutex       snapshotMtx_;
    std::vector<ChatMessage> snapshotMessages_;
};

}  // namespace x64ai
