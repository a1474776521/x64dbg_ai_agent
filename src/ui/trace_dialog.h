// ui/trace_dialog.h
//
// 调用链追溯对话框（v2 - Targeted/Global 双模式）：
//   - 顶部：模式 ComboBox（目标/全局-主动/全局-被动） + 地址输入框 + 解析按钮
//   - 当前目标行：显示已解析的 funcStart/funcEnd/label
//   - 控制条：● 启动 / ■ 停止 / ✕ 清空 / 🔄 刷新树
//   - 主体 QSplitter：左 调用树，右 详情面板
//   - 底部：让 AI 分析此调用链
//
// 入口：
//   - openForTarget(va)：菜单/外部调用，预填地址并自动解析为函数范围
#pragma once

#include <QDialog>

#include <cstdint>
#include <memory>

#include "trace/call_graph.h"
#include "trace/callstack_tracer.h"
#include "trace/trace_recorder.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTextEdit;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace x64ai {

class TraceDialog : public QDialog {
    Q_OBJECT
public:
    explicit TraceDialog(QWidget* parent = nullptr);
    ~TraceDialog() override;

    // 外部入口：用某个 VA 预填并尝试解析为函数范围（菜单/反汇编右键调用）。
    void openForTarget(uint64_t va, const QString& displayHint = {});

private slots:
    void onResolveClicked();
    void onModeChanged(int index);
    void onStartClicked();
    void onStopClicked();
    void onClearClicked();
    void onRefreshClicked();
    void onItemClicked(QTreeWidgetItem* item, int col);
    void onItemDoubleClicked(QTreeWidgetItem* item, int col);
    void onContextMenu(const QPoint& pos);
    void onAiAnalyze();

private:
    void rebuildTree();
    void rebuildStackTree();
    void populateViewRecursive(QTreeWidgetItem* parentItem, CallNodeView* node);
    void updateStatus();
    void updateTargetLabel();
    void updateButtons();
    QString buildPathText(QTreeWidgetItem* leaf) const;
    QString buildStackPathText(QTreeWidgetItem* leaf) const;
    bool    isCallStackMode() const;

    // 解析当前输入框 -> 写 currentTarget_；返回是否成功
    bool resolveInput();

    // 顶部：模式 + 地址
    QComboBox*   modeBox_       = nullptr;
    QLineEdit*   addrEdit_      = nullptr;
    QToolButton* resolveBtn_    = nullptr;
    QLabel*      targetLbl_     = nullptr;
    QSpinBox*    maxSamplesBox_ = nullptr;
    QLabel*      maxSamplesLbl_ = nullptr;

    // 控制条
    QToolButton* btnStart_      = nullptr;
    QToolButton* btnStop_       = nullptr;
    QToolButton* btnClear_      = nullptr;
    QToolButton* btnRefresh_    = nullptr;
    QCheckBox*   chkHideSys_    = nullptr;
    QCheckBox*   chkMerge_      = nullptr;
    QLabel*      statusLbl_     = nullptr;

    // 主体
    QStackedWidget* stack_      = nullptr;  // 0=正向调用树，1=反向调用栈采样树
    QTreeWidget* tree_          = nullptr;
    QTreeWidget* stackTree_     = nullptr;
    QTextEdit*   detail_        = nullptr;
    QPushButton* aiBtn_         = nullptr;

    std::unique_ptr<CallNode>     root_;       // 原始时序树（保留全部信息）
    std::unique_ptr<CallNodeView> view_;       // UI 折叠视图（重建自 root_）

    // 当前已解析目标（仅 Targeted 模式有效）
    TraceTarget  currentTarget_{};
    bool         targetResolved_ = false;
};

}  // namespace x64ai
