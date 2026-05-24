// ui/tool_call_card.h
//
// ToolCallCard：可折叠的 Agent 工具调用卡片。
//
// 状态机：
//   Pending  - 刚由 LLM 决定调用，工具还没派发（灰色 dot）
//   Running  - dispatch 中（蓝色 spinner 字符 ⟳）
//   Done     - 成功（绿色 ✓ + 耗时 + 截断标记）
//   Error    - 失败（红色 ✗ + error 文本）
//
// 视觉：
//   header： [icon] [tool name] [args 摘要] ... [status badge]   ← 可点击切换折叠
//   body：   Arguments: <json>
//            Result:    <json>     （error 时显示 Error: <msg>）
//
// 用 QLabel + QTextEdit；body 默认折叠。
#pragma once

#include <QFrame>
#include <QString>

class QLabel;
class QTextEdit;
class QToolButton;
class QHBoxLayout;
class QVBoxLayout;

namespace x64ai {

class ToolCallCard : public QFrame {
    Q_OBJECT
public:
    enum class State { Pending, Running, Done, Error };

    explicit ToolCallCard(const QString& id,
                          const QString& toolName,
                          QWidget*       parent = nullptr);

    QString id() const { return id_; }
    QString toolName() const { return toolName_; }

    // 流程 API（按发生顺序调用，多次调用安全；旧状态会被覆盖）
    void setPending();
    void setRunning(const QString& argsJson);
    void setDone(const QString& argsJson,
                 const QString& resultJson,
                 qint64         elapsedMs,
                 bool           truncated);
    void setError(const QString& argsJson,
                  const QString& error,
                  qint64         elapsedMs);

    void setExpanded(bool on);
    bool isExpanded() const;

private slots:
    void onHeaderClicked();

private:
    void rebuildHeader();
    void rebuildBody();
    QString digestArgs(const QString& argsJson) const;
    QString statusBadgeHtml() const;

    QString id_;
    QString toolName_;
    State   state_ = State::Pending;
    QString argsJson_;
    QString resultJson_;
    QString error_;
    qint64  elapsedMs_ = 0;
    bool    truncated_ = false;
    bool    expanded_  = false;

    QToolButton* toggleBtn_   = nullptr;   // ▶ / ▼
    QLabel*      nameLabel_   = nullptr;
    QLabel*      digestLabel_ = nullptr;
    QLabel*      statusLabel_ = nullptr;
    QTextEdit*   bodyEdit_    = nullptr;
};

}  // namespace x64ai
