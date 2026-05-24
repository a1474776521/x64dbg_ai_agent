// ui/locator_dialog.h
//
// 启发式定位器对话框：
//   - 顶部：复选框（API/String/Pattern）+ "扫描" 按钮 + 进度标签
//   - 中部：QTreeWidget 分组展示结果（按 kind > category），列：地址 / 标签 / 分数 / 证据
//   - 双击 hit -> 让 x64dbg 跳转该地址（DbgCmdExec("disasm 0x..." )）
//   - 右键 hit -> "让 AI 分析此处"（用 disasm_context 抓取 + sendChat）
#pragma once

#include <QDialog>

#include "locator/heuristic_hit.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace x64ai {

class LocatorDialog : public QDialog {
    Q_OBJECT
public:
    explicit LocatorDialog(QWidget* parent = nullptr);

private slots:
    void onScanClicked();
    void onItemDoubleClicked(QTreeWidgetItem* item, int col);
    void onContextMenu(const QPoint& pos);

private:
    void populateResults(const std::vector<HeuristicHit>& hits);

    QCheckBox*   cbApi_       = nullptr;
    QCheckBox*   cbString_    = nullptr;
    QCheckBox*   cbPattern_   = nullptr;
    QCheckBox*   cbWriteRag_  = nullptr;
    QLineEdit*   kwEdit_      = nullptr;
    QPushButton* scanBtn_     = nullptr;
    QLabel*      statusLbl_   = nullptr;
    QTreeWidget* tree_        = nullptr;
};

}  // namespace x64ai
