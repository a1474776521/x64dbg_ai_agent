// ui/api_key_dialog.cpp
#include "ui/api_key_dialog.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "ai/deepseek_chat_client.h"

namespace x64ai {

ApiKeyDialog::ApiKeyDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("DeepSeek API Key"));
    setMinimumWidth(480);

    // 继承父窗口主题 QSS（QSS 仅作用子树，需要显式设置）
    QFile f(QStringLiteral(":/x64dbg-ai/styles/theme_dark.qss"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setStyleSheet(QString::fromUtf8(f.readAll()));
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 12);
    root->setSpacing(10);

    auto* tip = new QLabel(
        QStringLiteral("请前往 <a href=\"https://platform.deepseek.com/\">"
                       "platform.deepseek.com</a> 注册账号并创建 API Key（以 sk- 开头）。"
                       "<br/>新用户注册赠送 10 元额度；按用量计费，价格极低。"
                       "<br/>Key 将通过 Windows DPAPI 加密存储在本地，仅当前用户可解密。"),
        this);
    tip->setOpenExternalLinks(true);
    tip->setWordWrap(true);
    root->addWidget(tip);

    // 当前 key 状态
    auto* statusRow = new QHBoxLayout();
    auto* statusKey = new QLabel(QStringLiteral("当前 Key："), this);
    maskLabel_ = new QLabel(this);
    maskLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusRow->addWidget(statusKey);
    statusRow->addWidget(maskLabel_, 1);
    root->addLayout(statusRow);
    refreshMaskLabel();

    // 新 key 输入
    auto* inputRow = new QHBoxLayout();
    auto* inputLab = new QLabel(QStringLiteral("新 Key："), this);
    edit_ = new QLineEdit(this);
    edit_->setEchoMode(QLineEdit::Password);
    edit_->setPlaceholderText(QStringLiteral("sk-..."));
    edit_->setClearButtonEnabled(true);

    eyeBtn_ = new QToolButton(this);
    eyeBtn_->setText(QStringLiteral("显示"));
    eyeBtn_->setCheckable(true);
    eyeBtn_->setToolTip(QStringLiteral("显示 / 隐藏 Key"));

    inputRow->addWidget(inputLab);
    inputRow->addWidget(edit_, 1);
    inputRow->addWidget(eyeBtn_);
    root->addLayout(inputRow);

    // 按钮区
    auto* btns = new QDialogButtonBox(this);
    auto* okBtn     = btns->addButton(QStringLiteral("保存"),     QDialogButtonBox::AcceptRole);
    auto* clearBtn  = btns->addButton(QStringLiteral("清除已保存"), QDialogButtonBox::DestructiveRole);
    auto* cancelBtn = btns->addButton(QStringLiteral("取消"),     QDialogButtonBox::RejectRole);
    Q_UNUSED(cancelBtn);
    okBtn->setDefault(true);
    root->addWidget(btns);

    connect(eyeBtn_,   &QToolButton::toggled,    this, &ApiKeyDialog::onToggleVisible);
    connect(clearBtn,  &QPushButton::clicked,    this, &ApiKeyDialog::onClear);
    connect(btns,      &QDialogButtonBox::accepted, this, &ApiKeyDialog::onAccept);
    connect(btns,      &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void ApiKeyDialog::refreshMaskLabel()
{
    auto m = DeepSeekChatClient::instance().maskedApiKey();
    if (m.empty()) {
        maskLabel_->setText(QStringLiteral("（未设置）"));
    } else {
        maskLabel_->setText(QString::fromStdString(m));
    }
}

void ApiKeyDialog::onToggleVisible()
{
    edit_->setEchoMode(eyeBtn_->isChecked() ? QLineEdit::Normal : QLineEdit::Password);
    eyeBtn_->setText(eyeBtn_->isChecked()
                         ? QStringLiteral("隐藏")
                         : QStringLiteral("显示"));
}

void ApiKeyDialog::onClear()
{
    auto ret = QMessageBox::question(
        this, QStringLiteral("确认清除"),
        QStringLiteral("确定要清除已保存的 DeepSeek API Key 吗？"),
        QMessageBox::Yes | QMessageBox::No);
    if (ret != QMessageBox::Yes) return;
    DeepSeekChatClient::instance().clearApiKey();
    cleared_ = true;
    refreshMaskLabel();
    edit_->clear();
}

void ApiKeyDialog::onAccept()
{
    QString k = edit_->text().trimmed();
    if (k.isEmpty()) {
        // 空输入但未清除 -> 视为取消
        if (cleared_) { accept(); return; }
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("请输入有效的 API Key（以 sk- 开头）。"));
        return;
    }
    if (!k.startsWith(QStringLiteral("sk-"))) {
        auto ret = QMessageBox::question(
            this, QStringLiteral("格式提醒"),
            QStringLiteral("DeepSeek API Key 通常以 sk- 开头。\n是否仍然保存这个值？"),
            QMessageBox::Yes | QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
    }
    accept();
}

QString ApiKeyDialog::apiKey() const
{
    return edit_ ? edit_->text().trimmed() : QString();
}

}  // namespace x64ai
