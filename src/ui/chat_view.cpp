// ui/chat_view.cpp
#include "ui/chat_view.h"

#include "ui/tool_call_card.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

namespace x64ai {

// ============================================================
// MessageBubble - 一条 user/assistant/system 消息的子控件
// ============================================================
class MessageBubble : public QFrame {
public:
    enum Role { User = 0, Assistant = 1, System = 2 };

    MessageBubble(Role role, const QString& text, QWidget* parent = nullptr)
        : QFrame(parent), role_(role)
    {
        setFrameShape(QFrame::NoFrame);
        label_ = new QLabel(this);
        label_->setTextFormat(Qt::RichText);
        label_->setWordWrap(true);
        label_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                        Qt::LinksAccessibleByMouse);

        QString styleSheet;
        Qt::Alignment align = Qt::AlignLeft;
        if (role == User) {
            styleSheet = QStringLiteral(
                "QLabel { background:#2a2d2e; color:#d4d4d4; "
                "border:1px solid #3c3c3c; padding:8px; }");
            align = Qt::AlignRight;
        } else if (role == System) {
            styleSheet = QStringLiteral(
                "QLabel { color:#7a7a7a; font-style:italic; font-size:8pt; }");
            align = Qt::AlignHCenter;
        } else {
            styleSheet = QStringLiteral(
                "QLabel { background:#252526; color:#d4d4d4; "
                "border:1px solid #3c3c3c; border-left:3px solid #007acc; "
                "padding:8px 10px; }");
            align = Qt::AlignLeft;
        }
        label_->setStyleSheet(styleSheet);
        // 限制气泡最大宽度（避免一行铺满）
        label_->setMaximumWidth(720);

        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(6, 4, 6, 4);
        if (align == Qt::AlignRight) {
            lay->addStretch(1);
            lay->addWidget(label_, 0, Qt::AlignTop);
        } else if (align == Qt::AlignHCenter) {
            lay->addStretch(1);
            lay->addWidget(label_, 0, Qt::AlignTop);
            lay->addStretch(1);
        } else {
            lay->addWidget(label_, 0, Qt::AlignTop);
            lay->addStretch(1);
        }

        setText(text);
    }

    void setText(const QString& s)
    {
        text_ = s;
        QString esc = s.toHtmlEscaped();
        esc.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
        if (esc.isEmpty()) esc = QStringLiteral("&nbsp;");
        label_->setText(esc);
    }
    void appendText(const QString& delta)
    {
        text_ += delta;
        setText(text_);
    }
    const QString& text() const { return text_; }
    Role role() const { return role_; }

private:
    Role     role_;
    QLabel*  label_ = nullptr;
    QString  text_;
};

// ============================================================
// ReasoningBlock - DeepSeek thinking 模型的 CoT 折叠面板
// 默认折叠；流式过程中"软实时"显示最后两行预览
// ============================================================
class ReasoningBlock : public QFrame {
public:
    explicit ReasoningBlock(QWidget* parent = nullptr) : QFrame(parent)
    {
        setFrameShape(QFrame::NoFrame);
        toggle_ = new QToolButton(this);
        toggle_->setText(QStringLiteral("\xE2\x96\xB6 \xE6\x80\x9D\xE8\x80\x83\xE8\xBF\x87\xE7\xA8\x8B")); // ▶ 思考过程
        toggle_->setCheckable(true);
        toggle_->setChecked(false);
        toggle_->setAutoRaise(true);
        toggle_->setCursor(Qt::PointingHandCursor);
        toggle_->setStyleSheet(QStringLiteral(
            "QToolButton { color:#9a9a9a; font-size:8pt; padding:2px 6px;"
            " border:none; background:transparent; text-align:left; }"
            "QToolButton:hover { color:#d4d4d4; }"));

        body_ = new QLabel(this);
        body_->setTextFormat(Qt::PlainText);
        body_->setWordWrap(true);
        body_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        body_->setMaximumWidth(720);
        body_->setStyleSheet(QStringLiteral(
            "QLabel { background:#202020; color:#9a9a9a; "
            "border:1px solid #2d2d2d; border-left:3px solid #6a6a6a; "
            "padding:6px 10px; font-family:Consolas,'Courier New',monospace; "
            "font-size:8pt; }"));
        body_->setVisible(false);

        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(6, 2, 6, 2);
        lay->setSpacing(2);
        lay->addWidget(toggle_, 0, Qt::AlignLeft);
        lay->addWidget(body_);

        QObject::connect(toggle_, &QToolButton::toggled, this, [this](bool on) {
            body_->setVisible(on);
            toggle_->setText(on
                ? QStringLiteral("\xE2\x96\xBC \xE6\x80\x9D\xE8\x80\x83\xE8\xBF\x87\xE7\xA8\x8B") // ▼ 思考过程
                : QStringLiteral("\xE2\x96\xB6 \xE6\x80\x9D\xE8\x80\x83\xE8\xBF\x87\xE7\xA8\x8B")); // ▶ 思考过程
        });
    }

    void appendText(const QString& delta)
    {
        text_ += delta;
        body_->setText(text_);
        // 即使折叠也显示字符数提示
        const int chars = text_.size();
        const QString label = toggle_->isChecked()
            ? QStringLiteral("\xE2\x96\xBC \xE6\x80\x9D\xE8\x80\x83\xE8\xBF\x87\xE7\xA8\x8B (%1)").arg(chars)
            : QStringLiteral("\xE2\x96\xB6 \xE6\x80\x9D\xE8\x80\x83\xE8\xBF\x87\xE7\xA8\x8B (%1)").arg(chars);
        toggle_->setText(label);
    }

    bool isEmpty() const { return text_.isEmpty(); }

private:
    QToolButton* toggle_ = nullptr;
    QLabel*      body_   = nullptr;
    QString      text_;
};

// ============================================================
// ChatView
// ============================================================

ChatView::ChatView(QWidget* parent) : QWidget(parent)
{
    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("chatTranscript"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setStyleSheet(QStringLiteral(
        "QScrollArea, QScrollArea > QWidget > QWidget { background:#1e1e1e; }"));

    streamHost_ = new QWidget();
    streamHost_->setStyleSheet(QStringLiteral("background:#1e1e1e;"));
    streamLayout_ = new QVBoxLayout(streamHost_);
    streamLayout_->setContentsMargins(8, 8, 8, 8);
    streamLayout_->setSpacing(4);
    streamLayout_->addStretch(1);   // 末尾占位，保证消息从顶部往下排
    scroll_->setWidget(streamHost_);

    input_ = new QPlainTextEdit(this);
    input_->setObjectName(QStringLiteral("chatInput"));
    input_->setPlaceholderText(QStringLiteral("在此输入问题，Ctrl+Enter 发送"));
    input_->setFixedHeight(96);
    input_->installEventFilter(this);

    sendBtn_ = new QPushButton(QStringLiteral("发送"), this);
    sendBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/send.svg")));
    sendBtn_->setCursor(Qt::PointingHandCursor);
    sendBtn_->setMinimumWidth(84);

    auto* bottom = new QHBoxLayout();
    bottom->setContentsMargins(0, 6, 0, 0);
    bottom->setSpacing(6);
    bottom->addWidget(input_, 1);
    bottom->addWidget(sendBtn_, 0, Qt::AlignBottom);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(0);
    root->addWidget(scroll_, 1);
    root->addLayout(bottom);

    flushTimer_ = new QTimer(this);
    flushTimer_->setSingleShot(true);
    flushTimer_->setInterval(16);
    connect(flushTimer_, &QTimer::timeout, this, &ChatView::onFlushTimeout);

    if (auto* sb = scroll_->verticalScrollBar()) {
        connect(sb, &QScrollBar::valueChanged,
                this, &ChatView::onScrollChanged);
    }

    connect(sendBtn_, &QPushButton::clicked, this, &ChatView::onSendClicked);
}

bool ChatView::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == input_ && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        const bool isEnter =
            (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter);
        if (isEnter && (ke->modifiers() & Qt::ControlModifier)) {
            onSendClicked();
            return true;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void ChatView::addWidgetToStream(QWidget* w)
{
    // 末尾 stretch 在 layout 最后一项；插到它之前
    const int idx = streamLayout_->count() - 1;
    streamLayout_->insertWidget(idx < 0 ? 0 : idx, w);
}

void ChatView::appendBubble(int role, const QString& text)
{
    auto* b = new MessageBubble(static_cast<MessageBubble::Role>(role), text, streamHost_);
    addWidgetToStream(b);
    scrollToBottomIfNeeded(/*force=*/true);
}

void ChatView::scrollToBottomIfNeeded(bool force)
{
    auto* sb = scroll_->verticalScrollBar();
    if (!sb) return;
    if (!force && !userAtBottom_) return;
    programmaticScroll_ = true;
    // 在子控件刚 insert 后，maximum 可能未更新；用 QTimer::singleShot 兜底
    sb->setValue(sb->maximum());
    QTimer::singleShot(0, this, [this]() {
        auto* sb = scroll_->verticalScrollBar();
        if (!sb) return;
        if (userAtBottom_) {
            programmaticScroll_ = true;
            sb->setValue(sb->maximum());
            programmaticScroll_ = false;
        }
    });
    programmaticScroll_ = false;
    userAtBottom_ = true;
}

void ChatView::onScrollChanged(int value)
{
    if (programmaticScroll_) return;
    auto* sb = scroll_->verticalScrollBar();
    if (!sb) return;
    userAtBottom_ = (value >= sb->maximum() - 4);
}

void ChatView::setInputEnabled(bool enabled)
{
    if (input_)   input_->setEnabled(enabled);
    if (sendBtn_) sendBtn_->setEnabled(enabled);
}

void ChatView::appendUserMessage(const QString& text)
{
    if (streamingBubble_) finalizeAssistantMessage();
    appendBubble(0 /*User*/, text);
}

void ChatView::appendAssistantHeader()
{
    if (streamingBubble_) finalizeAssistantMessage();
    streamingText_.clear();
    streamingBubble_ = new MessageBubble(MessageBubble::Assistant, QString(), streamHost_);
    addWidgetToStream(streamingBubble_);
    setInputEnabled(false);
    userAtBottom_ = true;
    scrollToBottomIfNeeded(/*force=*/true);
}

void ChatView::appendAssistantDelta(const QString& delta)
{
    if (delta.isEmpty()) return;
    if (!streamingBubble_) appendAssistantHeader();
    pendingDelta_.append(delta);
    if (!flushTimer_->isActive()) flushTimer_->start();
}

void ChatView::appendAssistantReasoningDelta(const QString& delta)
{
    if (delta.isEmpty()) return;
    // 若本轮还没有 reasoning 块，新建并插入消息流；位置在 assistant bubble 之前
    if (!streamingReasoning_) {
        // 若已经有 streamingBubble_（极端情况：content 先到了），则把 reasoning 仍插在末尾
        streamingReasoning_ = new ReasoningBlock(streamHost_);
        addWidgetToStream(streamingReasoning_);
    }
    streamingReasoning_->appendText(delta);
    scrollToBottomIfNeeded();
}

void ChatView::onFlushTimeout()
{
    flushPendingDelta();
}

void ChatView::flushPendingDelta()
{
    if (pendingDelta_.isEmpty()) return;
    QString local; local.swap(pendingDelta_);
    streamingText_ += local;
    if (streamingBubble_) {
        streamingBubble_->appendText(local);
    }
    scrollToBottomIfNeeded();
}

void ChatView::finalizeAssistantMessage()
{
    if (!streamingBubble_ && !streamingReasoning_) return;
    if (flushTimer_->isActive()) flushTimer_->stop();
    flushPendingDelta();
    // 内容为空（agent 模式 tool_calls-only 一轮）→ 删掉空气泡
    if (streamingBubble_ && streamingText_.trimmed().isEmpty()) {
        streamLayout_->removeWidget(streamingBubble_);
        streamingBubble_->deleteLater();
    }
    streamingBubble_ = nullptr;
    streamingText_.clear();
    // reasoning 块：保留在流中（用户事后仍可展开），仅清引用以便下一轮新建
    if (streamingReasoning_ && streamingReasoning_->isEmpty()) {
        streamLayout_->removeWidget(streamingReasoning_);
        streamingReasoning_->deleteLater();
    }
    streamingReasoning_ = nullptr;
    setInputEnabled(true);
}

void ChatView::appendSystemNote(const QString& text)
{
    if (streamingBubble_) finalizeAssistantMessage();
    appendBubble(2 /*System*/, text);
}

void ChatView::clearTranscript()
{
    // 全部子控件移除并 deleteLater；stretch 保留
    while (streamLayout_->count() > 1) {
        auto* it = streamLayout_->takeAt(0);
        if (!it) break;
        if (auto* w = it->widget()) w->deleteLater();
        delete it;
    }
    cards_.clear();
    streamingBubble_ = nullptr;
    streamingReasoning_ = nullptr;
    streamingText_.clear();
    pendingDelta_.clear();
    if (flushTimer_->isActive()) flushTimer_->stop();
    setInputEnabled(true);
}

ToolCallCard* ChatView::addToolCallCard(const QString& id, const QString& toolName)
{
    auto it = cards_.find(id);
    if (it != cards_.end()) return it.value();
    // 若正在流式中，先收尾本轮 assistant 气泡再插卡片，
    // 这样卡片直接挂在该轮 assistant 之后
    if (streamingBubble_) finalizeAssistantMessage();
    auto* card = new ToolCallCard(id, toolName, streamHost_);
    addWidgetToStream(card);
    cards_.insert(id, card);
    scrollToBottomIfNeeded(/*force=*/true);
    return card;
}

ToolCallCard* ChatView::findToolCallCard(const QString& id) const
{
    auto it = cards_.find(id);
    return it == cards_.end() ? nullptr : it.value();
}

void ChatView::appendInlineButton(const QString& text, std::function<void()> onClick)
{
    if (streamingBubble_) finalizeAssistantMessage();
    auto* host = new QFrame(streamHost_);
    host->setFrameShape(QFrame::NoFrame);
    auto* lay = new QHBoxLayout(host);
    lay->setContentsMargins(6, 4, 6, 4);
    auto* btn = new QPushButton(text, host);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(QStringLiteral(
        "QPushButton { background:#0e639c; color:white; border:none; "
        "padding:6px 14px; border-radius:3px; }"
        "QPushButton:hover { background:#1177bb; }"
        "QPushButton:disabled { background:#3c3c3c; color:#7a7a7a; }"));
    lay->addWidget(btn);
    lay->addStretch(1);
    addWidgetToStream(host);
    QObject::connect(btn, &QPushButton::clicked, host, [btn, host, cb = std::move(onClick)]() {
        btn->setEnabled(false);
        if (cb) cb();
        host->deleteLater();
    });
    scrollToBottomIfNeeded(/*force=*/true);
}

void ChatView::onSendClicked()
{
    const QString text = input_->toPlainText().trimmed();
    if (text.isEmpty()) return;
    input_->clear();
    appendUserMessage(text);
    emit userSubmitted(text);
}

}  // namespace x64ai
