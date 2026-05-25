// ui/preset_editor_dialog.h
//
// PresetEditorDialog：Agent 预设管理对话框（M4.6e）
//
// 功能：
//   - 左侧列表 + 右侧表单编辑全部字段
//   - 新建 / 复制 / 删除 / 恢复出厂 / 保存
//   - 工具白名单按钩选项分发，空集表示"全部"
//   - readonly 预设字段只读，但可"复制为新预设"
//
// 生命周期：modal 对话框；exec() 返回 QDialog::Accepted 表示有保存动作发生。
// 调用者：AssistantPanel::openPresetManager()。
#pragma once

#include "ai/agent_preset.h"

#include <QDialog>
#include <QVector>

class QTreeWidget;
class QTreeWidgetItem;
class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QPushButton;
class QLabel;
class QToolButton;

namespace x64ai {

class PresetEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit PresetEditorDialog(QWidget* parent = nullptr);

    // 是否产生过保存动作（用于调用者决定是否 rebuildAgentMenu / rebuildDisasmAiSubmenu）
    bool changed() const { return changed_; }

private slots:
    void onListCurrentChanged();
    void onNewClicked();
    void onCloneClicked();
    void onDeleteClicked();
    void onResetDefaultsClicked();
    void onSaveCurrentClicked();
    void onCloseClicked();
    void onToggleLockClicked();   // S9+：解锁/重新锁定 readonly 预设
    // S9++：表单化工具区
    void onToolsContextMenu(const QPoint& pos);
    void onToolItemDoubleClicked(QTreeWidgetItem* item, int column);
    // S9：工具勾选 UI
    void onToolSearchChanged(const QString& text);
    void onToolFilterChipToggled();
    void onToolGroupItemChanged(QTreeWidgetItem* item, int column);

private:
    void buildUi();
    void reloadList(const std::string& selectId = {});
    void populateForm(const AgentPreset& p);
    AgentPreset gatherForm() const;          // 从表单读出，不读 id/readonly
    void setFormEnabled(bool enabled);
    void markDirty(bool dirty = true);
    bool maybeAskDiscardChanges();           // 若 dirty_ 则弹确认；返回 true=允许丢弃
    void loadEnabledTools(const std::vector<std::string>& enabled);
    std::vector<std::string> collectEnabledTools() const;
    static QString makeListItemLabel(const AgentPreset& p);
    // S9：分组渲染
    void buildToolsTree();                    // 一次性构建 group → tool 三态树
    void applyToolFilter();                   // 根据 search + chip 隐藏/显示叶子
    void updateBadge(const AgentPreset& p);   // 右上 badge：工具数 · group · tags · provider · 🔒
    void updateToolsCount();                  // S9++：刷新「已勾选 N/M」
    void showToolDetails(QTreeWidgetItem* leaf);  // S9++：双击叶子弹完整详情

public:
    // 复用辅助（ToolsBrowserDialog 等需要）
    static QString prettyGroupName(const std::string& g);
    static QString prettyCategoryName(int cat);  // ToolCategory enum int

private:

    // 左侧 —— S9：QTreeWidget(group → preset)
    QTreeWidget* listWidget_ = nullptr;
    QPushButton* newBtn_     = nullptr;
    QPushButton* cloneBtn_   = nullptr;
    QPushButton* deleteBtn_  = nullptr;
    QPushButton* resetBtn_   = nullptr;

    // 右侧表单
    QLineEdit*       idEdit_         = nullptr;  // 只读展示
    QLineEdit*       nameEdit_       = nullptr;
    QLineEdit*       descEdit_       = nullptr;
    QComboBox*       providerBox_    = nullptr;
    QLineEdit*       modelEdit_      = nullptr;
    QSpinBox*        maxIterSpin_    = nullptr;
    QDoubleSpinBox*  tempSpin_       = nullptr;
    QCheckBox*       showCtxCheck_   = nullptr;
    QPlainTextEdit*  sysPromptEdit_  = nullptr;
    QPlainTextEdit*  userTplEdit_    = nullptr;
    // S9：预设分组 + 标签
    QComboBox*       groupBox_       = nullptr;  // general/exploration/cracking/tracing/scenarios
    QLineEdit*       tagsEdit_       = nullptr;  // 逗号分隔
    QLabel*          badgeLabel_     = nullptr;  // 顶部 badge
    QPushButton*     lockBtn_        = nullptr;  // S9+：解锁/重新锁定

    // 工具白名单 —— S9：QTreeWidget 三态 + 搜索 + chip
    QTreeWidget* toolsTree_      = nullptr;
    QLineEdit*   toolSearchEdit_ = nullptr;
    // chip 过滤按钮（toggle 风格）
    QToolButton* chipReadBtn_    = nullptr;
    QToolButton* chipCtrlBtn_    = nullptr;
    QToolButton* chipWriteBtn_   = nullptr;
    QPushButton* toolsAllBtn_    = nullptr;
    QPushButton* toolsNoneBtn_   = nullptr;
    QPushButton* toolsReadOnlyBtn_ = nullptr;  // 只勾 Read 类
    QLabel*      toolsCountLabel_  = nullptr;  // S9++：「已勾选 N/M」

    // 底部
    QPushButton* saveBtn_    = nullptr;
    QPushButton* closeBtn_   = nullptr;

    // 当前选中的预设 id 副本（用于保存时定位）
    std::string currentId_;
    bool        currentReadonly_ = false;
    bool        dirty_           = false;
    bool        changed_         = false;
    bool        suppressDirty_   = false;     // populateForm 期间屏蔽 markDirty
};

}  // namespace x64ai
