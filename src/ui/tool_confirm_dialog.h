// ui/tool_confirm_dialog.h
//
// S3-C：写工具执行前的二次确认对话框。
//
// 要点：
//   - 模态 QDialog，必须在 GUI 线程构造和 exec。
//   - 3 秒倒计时；倒计时未结束时"允许"按钮 disabled 并显示 "Allow (Ns)"。（K-37：5s→3s）
//   - 用户点拒绝、关闭、ESC 均视为拒绝。
//   - 不能 show modal-less，否则 agent 工具线程会立即继续。
//
// 线程模型：AgentWorker 后台线程通过 QMetaObject::invokeMethod(...,
// Qt::BlockingQueuedConnection) 把 confirmFromBackground 投到 GUI 线程跑。
#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QPushButton;
class QPlainTextEdit;
class QTimer;

namespace x64ai {

class ToolConfirmDialog : public QDialog {
    Q_OBJECT
public:
    // toolName: 工具名（窗口标题）
    // summary:  一句话描述本次操作的副作用（"在 0x401000 设置软件断点"）
    // argsJson: 详细参数 JSON（pretty 后展示，给高级用户看）
    // countdownSec: 倒计时秒数，默认 3（K-37：5→3，用户反馈 5s 太长）
    explicit ToolConfirmDialog(const QString& toolName,
                               const QString& summary,
                               const QString& argsJson,
                               int            countdownSec = 3,
                               QWidget*       parent       = nullptr);

    // 工具线程调用入口：阻塞跨线程显示 dialog，返回是否允许。
    // 内部用 BlockingQueuedConnection 投到 GUI 线程。如不在 GUI 线程也能安全调用。
    static bool confirmFromBackground(const QString& toolName,
                                      const QString& summary,
                                      const QString& argsJson,
                                      int            countdownSec = 3);

private slots:
    void onTick();

private:
    QLabel*         titleLabel_   = nullptr;
    QLabel*         summaryLabel_ = nullptr;
    QPlainTextEdit* argsView_     = nullptr;
    QPushButton*    allowButton_  = nullptr;
    QPushButton*    denyButton_   = nullptr;
    QTimer*         tick_         = nullptr;
    int             remaining_    = 0;
};

}  // namespace x64ai
