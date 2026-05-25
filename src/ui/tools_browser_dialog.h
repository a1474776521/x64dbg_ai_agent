// ui/tools_browser_dialog.h
//
// 只读「工具浏览」对话框（替代原 toolsBtn_ 的 popup menu）。
// - 分组树：12 组（与 PresetEditorDialog 同源）
// - 列：工具名 | 类别（彩色徽标）| 描述 | 启用（✓/✗，相对于当前激活预设）
// - 顶部 badge：共 N 个 · 当前预设启用 M 个 (绝对集合或 空集=全启用)
// - 搜索框 + Read/Ctrl/Write chip 过滤
// - 双击行 → 弹完整 schema/JSON 详情
// - 底部：「打开预设编辑器」「关闭」
// 注：本对话框不修改任何状态；点「打开预设编辑器」由调用方负责打开。
#pragma once

#include <QDialog>
#include <string>
#include <vector>

class QLineEdit;
class QLabel;
class QCheckBox;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace x64ai {

class ToolsBrowserDialog : public QDialog {
    Q_OBJECT
public:
    // activePresetId 可为空（表示"无激活预设"，此时全部工具视为可调用）
    explicit ToolsBrowserDialog(QWidget* parent, const std::string& activePresetId);

    // true = 用户点了「打开预设编辑器」；调用方据此 dlg.exec() 后跳预设编辑器
    bool openPresetEditorRequested() const { return openPresetEditor_; }

private slots:
    void onFilterChanged();
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onOpenPresetEditor();

private:
    void buildUi();
    void buildTree();        // 构建分组树骨架（含启用列）
    void applyFilter();      // 按 search + chip 过滤
    void updateBadge();      // 顶部 badge 文本
    void showToolDetails(QTreeWidgetItem* leaf);

    // 输入：激活预设的 enabledTools（空集 = 全启用）
    std::string activePresetId_;
    std::vector<std::string> enabledSet_;   // 当前预设启用集；空表示"无激活"
    bool        noActivePreset_ = false;    // activePresetId 为空时为 true
    bool        openPresetEditor_ = false;

    // 控件
    QLabel*       badgeLabel_     = nullptr;
    QLineEdit*    searchEdit_     = nullptr;
    QCheckBox*    chipRead_       = nullptr;
    QCheckBox*    chipCtrl_       = nullptr;
    QCheckBox*    chipWrite_      = nullptr;
    QCheckBox*    chipOnlyActive_ = nullptr;  // 只显示当前预设启用的
    QTreeWidget*  tree_           = nullptr;
    QPushButton*  openEditorBtn_  = nullptr;
};

}  // namespace x64ai
