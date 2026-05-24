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

class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QPushButton;

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

    // 左侧
    QListWidget* listWidget_ = nullptr;
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
    QListWidget*     toolsList_      = nullptr;  // 复选工具
    QPushButton*     toolsAllBtn_    = nullptr;
    QPushButton*     toolsNoneBtn_   = nullptr;

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
