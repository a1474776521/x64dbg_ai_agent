// ui/api_key_dialog.h
//
// DeepSeek API Key 输入弹窗。
//   - 显示当前已保存 key 的掩码（如 sk-12345678****abcd）
//   - 输入新 key 时左侧密码框可切换显隐
//   - 提供"清除已保存 key"按钮
#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QToolButton;

namespace x64ai {

class ApiKeyDialog : public QDialog {
    Q_OBJECT
public:
    explicit ApiKeyDialog(QWidget* parent = nullptr);

    // 用户输入的新 key（已 trim）；若用户只是点击"清除"则为空且 cleared() 为 true
    QString apiKey() const;
    bool    cleared() const { return cleared_; }

private slots:
    void onToggleVisible();
    void onClear();
    void onAccept();

private:
    void refreshMaskLabel();

    QLineEdit*   edit_       = nullptr;
    QToolButton* eyeBtn_     = nullptr;
    QLabel*      maskLabel_  = nullptr;
    bool         cleared_    = false;
};

}  // namespace x64ai
