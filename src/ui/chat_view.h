// ui/chat_view.h
//
// 聊天展示控件（Dark Modern 气泡风格 · QScrollArea + QVBoxLayout 版）：
//   - 上方滚动区，按时间顺序排列消息子控件（user 气泡 / assistant 气泡 /
//     system 注记 / ToolCallCard）
//   - 下方 QPlainTextEdit#chatInput + QPushButton#sendBtn，Ctrl+Enter 发送
//
// M4.6c 重构原因：旧版用单个 QTextEdit + setHtml，无法嵌入子 QWidget
// （ToolCallCard 折叠卡片）。新版每条消息一个 QFrame 子控件，
// Agent 运行时直接 addToolCallCard 即可。
#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

#include <functional>

class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QScrollBar;
class QTimer;
class QVBoxLayout;
class QLabel;

namespace x64ai {

class ToolCallCard;
class MessageBubble;  // 内部实现，cpp 中定义
class ReasoningBlock; // 内部实现，cpp 中定义

class ChatView : public QWidget {
    Q_OBJECT
public:
    explicit ChatView(QWidget* parent = nullptr);

    // ===== 旧版兼容 API（AssistantPanel 现有代码继续可用）=====
    void appendUserMessage(const QString& text);
    void appendAssistantHeader();          // 起一条新的 assistant 气泡（流式开始）
    void appendAssistantDelta(const QString& delta);
    // M4 Reasoning UI：DeepSeek thinking 模型的 CoT 流。当前轮第一次调用会
    // 在 streamLayout 末尾插入一个可折叠 ReasoningBlock；finalize 时清理引用。
    void appendAssistantReasoningDelta(const QString& delta);
    void finalizeAssistantMessage();       // 流结束
    void appendSystemNote(const QString& text);
    void clearTranscript();

    // ===== M4.6c 新增（Agent 模式用）=====
    // 新建一张 tool 卡片并插入消息流。若同 id 已存在，返回旧的。
    ToolCallCard* addToolCallCard(const QString& id, const QString& toolName);
    ToolCallCard* findToolCallCard(const QString& id) const;

    // 在消息流末尾追加"继续推理"内联按钮；点击后回调（按钮自动隐藏）。
    void appendInlineButton(const QString& text, std::function<void()> onClick);

signals:
    void userSubmitted(const QString& text);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private slots:
    void onSendClicked();
    void onFlushTimeout();
    void onScrollChanged(int value);

private:
    void appendBubble(int role, const QString& text);   // role: 0 user, 1 assistant, 2 system
    void scrollToBottomIfNeeded(bool force = false);
    void flushPendingDelta();
    void setInputEnabled(bool enabled);
    void addWidgetToStream(QWidget* w);

    QScrollArea*    scroll_      = nullptr;
    QWidget*        streamHost_  = nullptr;   // scroll 的 widget
    QVBoxLayout*    streamLayout_ = nullptr;   // 内含 spacer 在末尾
    QPlainTextEdit* input_       = nullptr;
    QPushButton*    sendBtn_     = nullptr;

    // 当前流式追加的 assistant bubble
    MessageBubble*  streamingBubble_ = nullptr;
    QString         streamingText_;
    QString         pendingDelta_;
    QTimer*         flushTimer_  = nullptr;

    // 当前轮 reasoning 块（thinking 模型）；assistantMessage 收尾时置空
    ReasoningBlock* streamingReasoning_ = nullptr;

    bool            userAtBottom_       = true;
    bool            programmaticScroll_ = false;

    QHash<QString, ToolCallCard*> cards_;
};

}  // namespace x64ai
