// ui/tool_call_card.cpp
#include "ui/tool_call_card.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

namespace x64ai {

namespace {

QString htmlEscape(const QString& s)
{
    QString r = s.toHtmlEscaped();
    r.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
    return r;
}

}  // namespace

ToolCallCard::ToolCallCard(const QString& id, const QString& toolName, QWidget* parent)
    : QFrame(parent),
      id_(id),
      toolName_(toolName)
{
    setObjectName(QStringLiteral("toolCallCard"));
    setFrameShape(QFrame::StyledPanel);
    setStyleSheet(QStringLiteral(
        "QFrame#toolCallCard {"
        "  background:#252526; border:1px solid #3c3c3c; border-radius:4px;"
        "}"
        "QFrame#toolCallCard QLabel { color:#d4d4d4; }"
        "QFrame#toolCallCard QTextEdit {"
        "  background:#1e1e1e; color:#d4d4d4;"
        "  border:1px solid #2d2d2d; border-radius:3px;"
        "  font-family:Consolas,\"Courier New\",monospace; font-size:9pt;"
        "}"
        "QFrame#toolCallCard QToolButton {"
        "  border:none; color:#7a7a7a; background:transparent;"
        "  padding:0 4px;"
        "}"
        "QFrame#toolCallCard QToolButton:hover { color:#d4d4d4; }"
    ));

    toggleBtn_ = new QToolButton(this);
    toggleBtn_->setText(QStringLiteral("▶"));
    toggleBtn_->setCursor(Qt::PointingHandCursor);
    toggleBtn_->setToolTip(QStringLiteral("展开/折叠"));
    connect(toggleBtn_, &QToolButton::clicked, this, &ToolCallCard::onHeaderClicked);

    nameLabel_   = new QLabel(this);
    digestLabel_ = new QLabel(this);
    statusLabel_ = new QLabel(this);

    nameLabel_->setTextFormat(Qt::RichText);
    digestLabel_->setTextFormat(Qt::RichText);
    statusLabel_->setTextFormat(Qt::RichText);

    digestLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(6, 4, 8, 4);
    header->setSpacing(6);
    header->addWidget(toggleBtn_, 0);
    header->addWidget(nameLabel_, 0);
    header->addWidget(digestLabel_, 1);
    header->addWidget(statusLabel_, 0);

    bodyEdit_ = new QTextEdit(this);
    bodyEdit_->setReadOnly(true);
    bodyEdit_->setMinimumHeight(60);
    bodyEdit_->setMaximumHeight(280);
    bodyEdit_->setLineWrapMode(QTextEdit::WidgetWidth);
    bodyEdit_->setVisible(false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addLayout(header);
    root->addWidget(bodyEdit_);

    setPending();
}

void ToolCallCard::onHeaderClicked()
{
    setExpanded(!expanded_);
}

void ToolCallCard::setExpanded(bool on)
{
    if (expanded_ == on) return;
    expanded_ = on;
    toggleBtn_->setText(on ? QStringLiteral("▼") : QStringLiteral("▶"));
    bodyEdit_->setVisible(on);
    rebuildBody();
}

bool ToolCallCard::isExpanded() const { return expanded_; }

QString ToolCallCard::digestArgs(const QString& argsJson) const
{
    QString s = argsJson.simplified();
    constexpr int kMax = 80;
    if (s.size() > kMax) s = s.left(kMax) + QStringLiteral("…");
    return s;
}

QString ToolCallCard::statusBadgeHtml() const
{
    switch (state_) {
    case State::Pending:
        return QStringLiteral("<span style='color:#7a7a7a;'>● 等待</span>");
    case State::Running:
        return QStringLiteral("<span style='color:#3794ff;'>⟳ 运行中</span>");
    case State::Done: {
        QString trunc = truncated_
            ? QStringLiteral(" <span style='color:#cca700;'>(截断)</span>")
            : QString();
        return QStringLiteral("<span style='color:#73c991;'>✓ %1ms</span>%2")
            .arg(elapsedMs_).arg(trunc);
    }
    case State::Error:
        return QStringLiteral("<span style='color:#f48771;'>✗ 失败 %1ms</span>")
            .arg(elapsedMs_);
    }
    return QString();
}

void ToolCallCard::rebuildHeader()
{
    nameLabel_->setText(QStringLiteral(
        "<span style='color:#9cdcfe;'>%1</span>").arg(htmlEscape(toolName_)));
    QString digest = htmlEscape(digestArgs(argsJson_));
    digestLabel_->setText(QStringLiteral(
        "<span style='color:#7a7a7a; font-family:Consolas,monospace; font-size:9pt;'>%1</span>")
        .arg(digest));
    statusLabel_->setText(statusBadgeHtml());
}

void ToolCallCard::rebuildBody()
{
    if (!expanded_) return;
    QString html = QStringLiteral(
        "<div style='font-family:Consolas,monospace; font-size:9pt; color:#d4d4d4;'>");
    if (!argsJson_.isEmpty()) {
        html += QStringLiteral(
            "<div style='color:#7a7a7a; margin:4px 0 2px 0;'>Arguments:</div>"
            "<pre style='margin:0; color:#ce9178;'>%1</pre>")
            .arg(htmlEscape(argsJson_));
    }
    if (state_ == State::Error) {
        html += QStringLiteral(
            "<div style='color:#f48771; margin:6px 0 2px 0;'>Error:</div>"
            "<pre style='margin:0; color:#f48771;'>%1</pre>")
            .arg(htmlEscape(error_));
    } else if (state_ == State::Done) {
        html += QStringLiteral(
            "<div style='color:#7a7a7a; margin:6px 0 2px 0;'>Result:</div>"
            "<pre style='margin:0; color:#d4d4d4;'>%1</pre>")
            .arg(htmlEscape(resultJson_));
    }
    html += QStringLiteral("</div>");
    bodyEdit_->setHtml(html);
}

void ToolCallCard::setPending()
{
    state_ = State::Pending;
    rebuildHeader();
    rebuildBody();
}

void ToolCallCard::setRunning(const QString& argsJson)
{
    state_    = State::Running;
    argsJson_ = argsJson;
    rebuildHeader();
    rebuildBody();
}

void ToolCallCard::setDone(const QString& argsJson,
                           const QString& resultJson,
                           qint64         elapsedMs,
                           bool           truncated)
{
    state_      = State::Done;
    argsJson_   = argsJson;
    resultJson_ = resultJson;
    elapsedMs_  = elapsedMs;
    truncated_  = truncated;
    rebuildHeader();
    rebuildBody();
}

void ToolCallCard::setError(const QString& argsJson,
                            const QString& error,
                            qint64         elapsedMs)
{
    state_     = State::Error;
    argsJson_  = argsJson;
    error_     = error;
    elapsedMs_ = elapsedMs;
    rebuildHeader();
    rebuildBody();
}

}  // namespace x64ai
