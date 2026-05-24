// ui/session_list.h
//
// AssistantPanel 左侧的会话列表面板：
//   - 显示当前 ProjectContext 的所有会话（标题+时间）
//   - 双击切换；右键重命名/删除；顶部"新建"按钮
//
// 选中会话时发出 sessionSelected(int64_t)；未在调试或无 store 时禁用。
#pragma once

#include <cstdint>
#include <QWidget>

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QLabel;

namespace x64ai {

class SessionListWidget : public QWidget {
    Q_OBJECT
public:
    explicit SessionListWidget(QWidget* parent = nullptr);

    // 当前 store 变化（开始/停止调试）后，外部调用以刷新列表。
    void refresh();

    // 当前选中的会话 id；无则 0。
    int64_t currentSessionId() const { return currentId_; }

signals:
    void sessionSelected(qint64 id);
    void sessionsChanged();   // 创建/删除/重命名后

private slots:
    void onCreateClicked();
    void onItemActivated(QListWidgetItem* item);
    void onContextMenu(const QPoint& pos);

private:
    QLabel*       header_  = nullptr;
    QListWidget*  list_    = nullptr;
    QPushButton*  newBtn_  = nullptr;
    int64_t       currentId_ = 0;
};

}  // namespace x64ai
