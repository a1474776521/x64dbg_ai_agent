// ui/history_dialog.h
//
// 跨 DB 会话浏览器对话框（M3.6）：
//   左：历史 .db 列表（SHA 前 12 + 大小 + mtime）；当前活动项目高亮
//   中：所选 db 的 sessions 列表
//   右：所选 session 的 messages 预览
//   底：导入到当前项目（拷 messages 到当前 SessionStore 新建会话）
//
// 关闭时若发生过导入，emit imported() 让调用方刷新会话面板。
#pragma once

#include <QDialog>
#include <cstdint>
#include <filesystem>
#include <vector>

class QListWidget;
class QListWidgetItem;
class QTableWidget;
class QTextBrowser;
class QPushButton;
class QLabel;

namespace x64ai {

struct ProjectDbInfo;

class HistoryDialog : public QDialog {
    Q_OBJECT
public:
    explicit HistoryDialog(QWidget* parent = nullptr);

signals:
    // 导入成功后发出（一次或多次），调用方据此刷新左侧会话列表。
    void imported();

private slots:
    void onDbSelected(int row);
    void onSessionSelected(int row, int col);
    void onImportClicked();

private:
    void reloadDbList();
    void reloadSessionTable(const std::filesystem::path& dbPath);
    void reloadMessagePreview(const std::filesystem::path& dbPath, std::int64_t sessionId);

    QListWidget*  dbList_       = nullptr;
    QTableWidget* sessionTable_ = nullptr;
    QTextBrowser* preview_      = nullptr;
    QPushButton*  importBtn_    = nullptr;
    QPushButton*  closeBtn_     = nullptr;
    QLabel*       hint_         = nullptr;

    std::vector<ProjectDbInfo> dbs_;
    int           currentDbRow_      = -1;
    std::int64_t  currentSessionId_  = 0;
    std::string   currentSessionTitle_;
    std::string   currentSessionModel_;
};

}  // namespace x64ai
