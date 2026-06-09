// ui/kspmas_api_key_dialog.cpp
#include "ui/kspmas_api_key_dialog.h"

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

#include "ai/kspmas_chat_client.h"

namespace x64ai {

KSPmasApiKeyDialog::KSPmasApiKeyDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("金山云 KSPmas API Key"));
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
        QStringLiteral("请前往 <a href=\"https://www.ksyun.com/\">金山云控制台</a> "
                       "开通 KSPmas（大模型推理服务）并获取 API Key。"
                       "<br/>默认模型：deepseek-v4-pro（如需切换，请在模型下拉中选择或在 Preset 中指定）。"
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
    edit_->setPlaceholderText(QStringLiteral("粘贴 KSPmas API Key"));
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

    connect(eyeBtn_,   &QToolButton::toggled,    this, &KSPmasApiKeyDialog::onToggleVisible);
    connect(clearBtn,  &QPushButton::clicked,    this, &KSPmasApiKeyDialog::onClear);
    connect(btns,      &QDialogButtonBox::accepted, this, &KSPmasApiKeyDialog::onAccept);
    connect(btns,      &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void KSPmasApiKeyDialog::refreshMaskLabel()
{
    auto m = KSPmasChatClient::instance().maskedApiKey();
    if (m.empty()) {
        maskLabel_->setText(QStringLiteral("（未设置）"));
    } else {
        maskLabel_->setText(QString::fromStdString(m));
    }
}

void KSPmasApiKeyDialog::onToggleVisible()
{
    edit_->setEchoMode(eyeBtn_->isChecked() ? QLineEdit::Normal : QLineEdit::Password);
    eyeBtn_->setText(eyeBtn_->isChecked()
                         ? QStringLiteral("隐藏")
                         : QStringLiteral("显示"));
}

void KSPmasApiKeyDialog::onClear()
{
    auto ret = QMessageBox::question(
        this, QStringLiteral("确认清除"),
        QStringLiteral("确定要清除已保存的 KSPmas API Key 吗？"),
        QMessageBox::Yes | QMessageBox::No);
    if (ret != QMessageBox::Yes) return;
    KSPmasChatClient::instance().clearApiKey();
    cleared_ = true;
    refreshMaskLabel();
    edit_->clear();
}

void KSPmasApiKeyDialog::onAccept()
{
    QString k = edit_->text().trimmed();
    if (k.isEmpty()) {
        // 空输入但未清除 -> 视为取消
        if (cleared_) { accept(); return; }
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("请输入 KSPmas API Key。"));
        return;
    }
    // 长度软校验（防止误粘贴整段邮件等）
    if (k.size() < 8 || k.size() > 256) {
        auto ret = QMessageBox::question(
            this, QStringLiteral("格式提醒"),
            QStringLiteral("Key 长度不太常见（%1 字符）。\n是否仍然保存？").arg(k.size()),
            QMessageBox::Yes | QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
    }
    accept();
}

QString KSPmasApiKeyDialog::apiKey() const
{
    return edit_ ? edit_->text().trimmed() : QString();
}

}  // namespace x64ai
