// ui/login_dialog.h
//
// GitHub Device Flow 登录弹窗。
// 显示 user_code 与 verification_uri，提供复制 / 打开浏览器按钮，
// 后台线程按服务器返回的 interval 轮询直到授权完成、超时或被拒绝。
#pragma once

#include <QDialog>
#include <atomic>
#include <memory>

class QLabel;
class QPushButton;
class QProgressBar;

namespace x64ai {

struct DeviceCodeInfo;

class LoginDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(QWidget* parent = nullptr);
    ~LoginDialog() override;

    // 阻塞式入口：内部调用 exec()，成功返回 true 并已落盘 oauth_token。
    bool runLogin();

private slots:
    void onCopyCode();
    void onOpenBrowser();
    void onCancelClicked();

private:
    void startPolling(const DeviceCodeInfo& info);
    void appendStatus(const QString& msg);
    void finishSuccess(const QString& login);
    void finishFailure(const QString& reason);

    QLabel*       userCodeLabel_   = nullptr;
    QLabel*       verifyUriLabel_  = nullptr;
    QLabel*       statusLabel_     = nullptr;
    QProgressBar* progress_        = nullptr;
    QPushButton*  copyBtn_         = nullptr;
    QPushButton*  openBtn_         = nullptr;
    QPushButton*  cancelBtn_       = nullptr;

    QString                 userCode_;
    QString                 verifyUri_;
    std::shared_ptr<std::atomic_bool> cancelFlag_;
    bool                    success_ = false;
};

}  // namespace x64ai
