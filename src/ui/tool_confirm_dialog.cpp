// ui/tool_confirm_dialog.cpp
#include "ui/tool_confirm_dialog.h"

#include "util/logging.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace x64ai {

ToolConfirmDialog::ToolConfirmDialog(const QString& toolName,
                                     const QString& summary,
                                     const QString& argsJson,
                                     int            countdownSec,
                                     QWidget*       parent)
    : QDialog(parent)
    , remaining_(countdownSec)
{
    setWindowTitle(QStringLiteral("AI 工具确认：%1").arg(toolName));
    setModal(true);
    setMinimumWidth(520);

    titleLabel_ = new QLabel(QStringLiteral("Agent 请求执行写工具"), this);
    {
        QFont f = titleLabel_->font();
        f.setBold(true);
        f.setPointSize(f.pointSize() + 1);
        titleLabel_->setFont(f);
    }
    summaryLabel_ = new QLabel(summary, this);
    summaryLabel_->setWordWrap(true);

    argsView_ = new QPlainTextEdit(this);
    argsView_->setReadOnly(true);
    argsView_->setPlainText(argsJson);
    argsView_->setMaximumHeight(180);
    argsView_->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { font-family: Consolas, 'Cascadia Mono', monospace; "
        "font-size: 11px; background: #1e1e1e; color: #d4d4d4; border: 1px solid #444; }"));

    allowButton_ = new QPushButton(this);
    denyButton_  = new QPushButton(QStringLiteral("拒绝 (Esc)"), this);
    allowButton_->setEnabled(false);
    allowButton_->setText(QStringLiteral("允许 (%1s)").arg(remaining_));
    denyButton_->setDefault(true);  // ESC / Enter 默认拒绝

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->addWidget(denyButton_);
    btnRow->addWidget(allowButton_);

    auto* root = new QVBoxLayout(this);
    root->addWidget(titleLabel_);
    root->addWidget(summaryLabel_);
    root->addWidget(new QLabel(QStringLiteral("参数："), this));
    root->addWidget(argsView_);
    root->addLayout(btnRow);

    connect(allowButton_, &QPushButton::clicked, this, &QDialog::accept);
    connect(denyButton_,  &QPushButton::clicked, this, &QDialog::reject);

    tick_ = new QTimer(this);
    tick_->setInterval(1000);
    connect(tick_, &QTimer::timeout, this, &ToolConfirmDialog::onTick);
    tick_->start();
}

void ToolConfirmDialog::onTick()
{
    --remaining_;
    if (remaining_ <= 0) {
        tick_->stop();
        allowButton_->setEnabled(true);
        allowButton_->setText(QStringLiteral("允许"));
    } else {
        allowButton_->setText(QStringLiteral("允许 (%1s)").arg(remaining_));
    }
}

// 静态：跨线程入口
bool ToolConfirmDialog::confirmFromBackground(const QString& toolName,
                                              const QString& summary,
                                              const QString& argsJson,
                                              int            countdownSec)
{
    QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app) {
        // 无 GUI（极端情况，如插件刚卸载）→ 安全起见拒绝
        XAI_LOG_WARN("ToolConfirmDialog: no QApplication; deny by default ({})",
                     toolName.toStdString());
        return false;
    }

    bool accepted = false;
    auto runOnGui = [&]() {
        ToolConfirmDialog dlg(toolName, summary, argsJson, countdownSec,
                              app->activeWindow());
        accepted = (dlg.exec() == QDialog::Accepted);
    };

    if (QThread::currentThread() == app->thread()) {
        // 已在 GUI 线程（极少；理论上 dispatch 总在工具线程）
        runOnGui();
    } else {
        QMetaObject::invokeMethod(app, runOnGui, Qt::BlockingQueuedConnection);
    }
    return accepted;
}

}  // namespace x64ai
