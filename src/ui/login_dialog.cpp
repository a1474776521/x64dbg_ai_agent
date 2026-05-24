// ui/login_dialog.cpp
#include "ui/login_dialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include "ai/copilot_auth.h"
#include "util/logging.h"

namespace x64ai {

LoginDialog::LoginDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("GitHub Copilot 登录"));
    setModal(true);
    setMinimumWidth(440);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto* intro = new QLabel(
        QStringLiteral("请在浏览器中打开下方地址，输入下方一次性验证码完成授权。\n"
                       "授权成功后本窗口会自动关闭。"),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    // 验证码（大字号显眼）
    userCodeLabel_ = new QLabel(QStringLiteral("------"), this);
    QFont f = userCodeLabel_->font();
    f.setPointSize(f.pointSize() + 8);
    f.setBold(true);
    userCodeLabel_->setFont(f);
    userCodeLabel_->setAlignment(Qt::AlignCenter);
    userCodeLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(userCodeLabel_);

    auto* codeBtnRow = new QHBoxLayout();
    copyBtn_ = new QPushButton(QStringLiteral("复制验证码"), this);
    openBtn_ = new QPushButton(QStringLiteral("打开浏览器"), this);
    codeBtnRow->addStretch(1);
    codeBtnRow->addWidget(copyBtn_);
    codeBtnRow->addWidget(openBtn_);
    codeBtnRow->addStretch(1);
    root->addLayout(codeBtnRow);

    verifyUriLabel_ = new QLabel(QStringLiteral("https://github.com/login/device"), this);
    verifyUriLabel_->setAlignment(Qt::AlignCenter);
    verifyUriLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(verifyUriLabel_);

    progress_ = new QProgressBar(this);
    progress_->setRange(0, 0);  // 不确定进度
    root->addWidget(progress_);

    statusLabel_ = new QLabel(QStringLiteral("正在请求设备验证码..."), this);
    statusLabel_->setWordWrap(true);
    root->addWidget(statusLabel_);

    auto* btnRow = new QHBoxLayout();
    cancelBtn_ = new QPushButton(QStringLiteral("取消"), this);
    btnRow->addStretch(1);
    btnRow->addWidget(cancelBtn_);
    root->addLayout(btnRow);

    connect(copyBtn_,   &QPushButton::clicked, this, &LoginDialog::onCopyCode);
    connect(openBtn_,   &QPushButton::clicked, this, &LoginDialog::onOpenBrowser);
    connect(cancelBtn_, &QPushButton::clicked, this, &LoginDialog::onCancelClicked);

    cancelFlag_ = std::make_shared<std::atomic_bool>(false);
}

LoginDialog::~LoginDialog()
{
    if (cancelFlag_) cancelFlag_->store(true);
}

bool LoginDialog::runLogin()
{
    // 先在后台请求 device code，再在 UI 显示
    auto* self = this;
    QtConcurrent::run([self]() {
        auto info = CopilotAuth::instance().beginDeviceLogin();
        QMetaObject::invokeMethod(self, [self, info]() {
            if (!info) {
                self->finishFailure(
                    QStringLiteral("无法获取设备验证码，请检查网络后重试。"));
                return;
            }
            self->userCode_  = QString::fromStdString(info->userCode);
            self->verifyUri_ = QString::fromStdString(info->verificationUri);
            self->userCodeLabel_->setText(self->userCode_);
            self->verifyUriLabel_->setText(self->verifyUri_);
            self->appendStatus(QStringLiteral(
                "请打开 %1 并输入上方验证码。").arg(self->verifyUri_));
            self->startPolling(*info);
        }, Qt::QueuedConnection);
    });

    exec();
    return success_;
}

void LoginDialog::onCopyCode()
{
    if (userCode_.isEmpty()) return;
    QApplication::clipboard()->setText(userCode_);
    appendStatus(QStringLiteral("验证码已复制到剪贴板。"));
}

void LoginDialog::onOpenBrowser()
{
    if (verifyUri_.isEmpty()) return;
    QDesktopServices::openUrl(QUrl(verifyUri_));
}

void LoginDialog::onCancelClicked()
{
    if (cancelFlag_) cancelFlag_->store(true);
    success_ = false;
    reject();
}

void LoginDialog::appendStatus(const QString& msg)
{
    statusLabel_->setText(msg);
}

void LoginDialog::finishSuccess(const QString& login)
{
    success_ = true;
    appendStatus(QStringLiteral("登录成功：%1").arg(login));
    progress_->setRange(0, 1);
    progress_->setValue(1);
    accept();
}

void LoginDialog::finishFailure(const QString& reason)
{
    success_ = false;
    appendStatus(QStringLiteral("登录失败：%1").arg(reason));
    progress_->setRange(0, 1);
    progress_->setValue(0);
    cancelBtn_->setText(QStringLiteral("关闭"));
}

void LoginDialog::startPolling(const DeviceCodeInfo& info)
{
    auto* self     = this;
    auto  cancel   = cancelFlag_;
    std::string deviceCode = info.deviceCode;
    int interval = info.intervalSec > 0 ? info.intervalSec : 5;
    int expires  = info.expiresInSec  > 0 ? info.expiresInSec  : 900;

    QtConcurrent::run([self, cancel, deviceCode, interval, expires]() {
        int waited = 0;
        int curInterval = interval;
        while (!cancel->load() && waited < expires) {
            QThread::sleep(static_cast<unsigned long>(curInterval));
            waited += curInterval;
            if (cancel->load()) return;

            auto res = CopilotAuth::instance().pollDeviceLogin(deviceCode);
            if (res.state == DevicePollState::Pending) {
                continue;
            }
            if (res.state == DevicePollState::SlowDown) {
                curInterval += 5;
                continue;
            }
            if (res.state == DevicePollState::Authorized) {
                bool ok = CopilotAuth::instance().persistOAuthToken(res.accessToken);
                QMetaObject::invokeMethod(self, [self, ok]() {
                    if (ok) {
                        auto user = CopilotAuth::instance().currentUserLogin();
                        self->finishSuccess(user
                            ? QString::fromStdString(*user)
                            : QStringLiteral("(未知用户)"));
                    } else {
                        self->finishFailure(QStringLiteral("token 写入磁盘失败"));
                    }
                }, Qt::QueuedConnection);
                return;
            }
            // 其它失败态
            QString reason = QString::fromStdString(res.errorMessage);
            QMetaObject::invokeMethod(self, [self, reason]() {
                self->finishFailure(reason.isEmpty()
                    ? QStringLiteral("未知错误")
                    : reason);
            }, Qt::QueuedConnection);
            return;
        }
        if (!cancel->load()) {
            QMetaObject::invokeMethod(self, [self]() {
                self->finishFailure(QStringLiteral("等待超时，请重试。"));
            }, Qt::QueuedConnection);
        }
    });
}

}  // namespace x64ai
