// ai/agent_worker.cpp

#include "ai/agent_worker.h"

#include "ai/agent_loop.h"
#include "storage/project_context.h"
#include "storage/session_store.h"
#include "ui/tool_confirm_dialog.h"
#include "util/logging.h"

#include <bridgemain.h>

#include <QPointer>
#include <QtConcurrent/QtConcurrent>

namespace x64ai {

AgentWorker::AgentWorker(QObject* parent)
    : QObject(parent),
      cancel_(std::make_shared<std::atomic<bool>>(false))
{
}

AgentWorker::~AgentWorker()
{
    requestCancel();
}

void AgentWorker::requestCancel()
{
    if (cancel_) cancel_->store(true);
}

void AgentWorker::start(AgentRunRequest req)
{
    if (running_.exchange(true)) {
        XAI_LOG_WARN("AgentWorker::start called while already running; ignored");
        return;
    }
    cancel_->store(false);

    // 拷一份到 lambda；用 QPointer 避免 worker 提前销毁
    QPointer<AgentWorker> self(this);
    auto cancel = cancel_;
    QtConcurrent::run([self, cancel, req = std::move(req)]() mutable {
        // 构造 ToolContext
        ToolContext ctx;
        auto store = ProjectContext::instance().store();
        ctx.sessionStore   = store ? reinterpret_cast<void*>(store.get()) : nullptr;
        ctx.targetSha      = ProjectContext::instance().projectId();
        ctx.debuggerActive = DbgIsDebugging();
        ctx.cancelFlag     = cancel.get();  // S2-D：让工具内阻塞循环能响应用户取消

        // S3-D：注入跨线程模态 confirm 回调。dispatch 在工具线程调用本 lambda 时，
        // ToolConfirmDialog 内部会 BlockingQueuedConnection 切回 GUI 线程。
        ctx.confirmCallback = [](const std::string& toolName,
                                  const std::string& summary,
                                  const std::string& argsPretty) -> bool {
            return ToolConfirmDialog::confirmFromBackground(
                QString::fromStdString(toolName),
                QString::fromStdString(summary),
                QString::fromStdString(argsPretty),
                /*countdownSec=*/5);
        };

        AgentRunCallbacks cb;
        cb.onAssistantDelta = [self](std::string_view d) {
            if (!self) return;
            QString s = QString::fromUtf8(d.data(), static_cast<int>(d.size()));
            QMetaObject::invokeMethod(self.data(),
                [self, s]() { if (self) emit self->assistantDelta(s); },
                Qt::QueuedConnection);
        };
        cb.onAssistantReasoningDelta = [self](std::string_view d) {
            if (!self) return;
            QString s = QString::fromUtf8(d.data(), static_cast<int>(d.size()));
            QMetaObject::invokeMethod(self.data(),
                [self, s]() { if (self) emit self->assistantReasoningDelta(s); },
                Qt::QueuedConnection);
        };
        cb.onAssistantMessage = [self](const ChatMessage& m) {
            if (!self) return;
            QString content = QString::fromStdString(m.content);
            QStringList ids, names;
            ids.reserve(static_cast<int>(m.toolCalls.size()));
            names.reserve(static_cast<int>(m.toolCalls.size()));
            for (const auto& tc : m.toolCalls) {
                ids   << QString::fromStdString(tc.id);
                names << QString::fromStdString(tc.name);
            }
            QMetaObject::invokeMethod(self.data(),
                [self, content, ids, names]() {
                    if (self) emit self->assistantMessage(content, ids, names);
                },
                Qt::QueuedConnection);
        };
        cb.onToolReport = [self](const AgentToolCallReport& r) {
            if (!self) return;
            QString id      = QString::fromStdString(r.id);
            QString name    = QString::fromStdString(r.name);
            QString args    = QString::fromStdString(r.argumentsJson);
            QString result  = QString::fromStdString(r.resultJson);
            QString err     = QString::fromStdString(r.error);
            bool    ok      = r.ok;
            qint64  elapsed = static_cast<qint64>(r.elapsedMs);
            bool    trunc   = r.truncated;
            QMetaObject::invokeMethod(self.data(),
                [self, id, name, args, result, ok, err, elapsed, trunc]() {
                    if (!self) return;
                    emit self->toolCallStarted(id, name, args);
                    emit self->toolCallFinished(id, name, args, result, ok, err, elapsed, trunc);
                },
                Qt::QueuedConnection);
        };
        cb.onError = [self](const std::string& e) {
            if (!self) return;
            QString s = QString::fromStdString(e);
            QMetaObject::invokeMethod(self.data(),
                [self, s]() { if (self) emit self->failed(s); },
                Qt::QueuedConnection);
        };
        cb.onMaxIterReached = [self](int it, int pending) {
            if (!self) return;
            QMetaObject::invokeMethod(self.data(),
                [self, it, pending]() {
                    if (self) emit self->maxIterReached(it, pending);
                },
                Qt::QueuedConnection);
        };
        // G-2 (2026-05-25): prompt cache 命中观测
        cb.onUsage = [self](const UsageInfo& u) {
            if (!self) return;
            // [G-2 CACHE] 单行日志，便于 grep；hit_ratio 在 prompt=0 时记 "n/a"
            double r = u.hitRatio();
            if (r < 0.0) {
                XAI_LOG_INFO(
                    "[G-2 CACHE] input={} cached=0 miss={} hit_ratio=n/a completion={} reasoning={} cache_creation={}",
                    u.promptTokens, u.promptTokens, u.completionTokens,
                    u.reasoningTokens, u.cacheCreationTokens);
            } else {
                XAI_LOG_INFO(
                    "[G-2 CACHE] input={} cached={} miss={} hit_ratio={:.1f}% completion={} reasoning={} cache_creation={}",
                    u.promptTokens, u.cachedPromptTokens,
                    u.promptTokens - u.cachedPromptTokens, r * 100.0,
                    u.completionTokens, u.reasoningTokens, u.cacheCreationTokens);
            }
            int pt = u.promptTokens, ct = u.cachedPromptTokens,
                comp = u.completionTokens, rt = u.reasoningTokens;
            double ratio = r;
            QMetaObject::invokeMethod(self.data(),
                [self, pt, ct, comp, rt, ratio]() {
                    if (self) emit self->usageUpdated(pt, ct, comp, rt, ratio);
                },
                Qt::QueuedConnection);
        };
        cb.onDone = [self]() {
            if (!self) return;
            // 不在这里 emit finished —— iters 此时尚未返回；
            // 统一在 AgentLoop::run 返回后 emit finished(iters)。
        };

        int iters = 0;
        try {
            iters = AgentLoop::run(req, ctx, cb, *cancel);
        } catch (const std::exception& e) {
            QString s = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(self.data(),
                [self, s]() { if (self) emit self->failed(QStringLiteral("agent threw: ") + s); },
                Qt::QueuedConnection);
        } catch (...) {
            QMetaObject::invokeMethod(self.data(),
                [self]() { if (self) emit self->failed(QStringLiteral("agent threw unknown")); },
                Qt::QueuedConnection);
        }

        // 更新 running 标记 + 统一 emit finished(iters)
        QMetaObject::invokeMethod(self.data(),
            [self, iters]() {
                if (!self) return;
                self->running_.store(false);
                emit self->finished(iters);
            },
            Qt::QueuedConnection);
    });
}

}  // namespace x64ai
