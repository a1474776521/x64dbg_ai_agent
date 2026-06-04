// ui/history_dialog.cpp
#include "ui/history_dialog.h"

#include <QDateTime>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "storage/project_browser.h"
#include "storage/project_context.h"
#include "storage/session_store.h"
#include "util/logging.h"

namespace x64ai {

namespace {

QString formatSize(std::uint64_t bytes) {
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    double kb = bytes / 1024.0;
    if (kb < 1024.0) return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(kb / 1024.0, 0, 'f', 2);
}

QString formatEpoch(std::int64_t sec) {
    if (sec <= 0) return QStringLiteral("—");
    return QDateTime::fromSecsSinceEpoch(sec).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

}  // namespace

HistoryDialog::HistoryDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("历史项目会话浏览器"));
    resize(1100, 640);

    // 继承父窗口 QSS（QDialog 默认会继承，但显式触发一次 polish 更稳）
    if (parent) {
        setStyleSheet(parent->styleSheet());
    }

    // ---- 左：DB 列表 ----
    dbList_ = new QListWidget(this);
    dbList_->setMinimumWidth(260);
    dbList_->setMaximumWidth(360);
    dbList_->setUniformItemSizes(false);
    dbList_->setAlternatingRowColors(false);

    auto* leftWrap = new QWidget(this);
    auto* leftLay  = new QVBoxLayout(leftWrap);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->setSpacing(4);
    auto* leftHdr = new QLabel(QStringLiteral("历史项目库"), leftWrap);
    leftHdr->setObjectName(QStringLiteral("sectionHeader"));
    leftLay->addWidget(leftHdr);
    leftLay->addWidget(dbList_, 1);

    // ---- 中：sessions 表 ----
    sessionTable_ = new QTableWidget(this);
    sessionTable_->setColumnCount(4);
    sessionTable_->setHorizontalHeaderLabels(
        QStringList() << QStringLiteral("ID")
                      << QStringLiteral("标题")
                      << QStringLiteral("模型")
                      << QStringLiteral("最后更新"));
    sessionTable_->horizontalHeader()->setStretchLastSection(false);
    sessionTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    sessionTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    sessionTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    sessionTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    sessionTable_->verticalHeader()->setVisible(false);
    sessionTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    sessionTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    sessionTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto* midWrap = new QWidget(this);
    auto* midLay  = new QVBoxLayout(midWrap);
    midLay->setContentsMargins(0, 0, 0, 0);
    midLay->setSpacing(4);
    auto* midHdr = new QLabel(QStringLiteral("会话"), midWrap);
    midHdr->setObjectName(QStringLiteral("sectionHeader"));
    midLay->addWidget(midHdr);
    midLay->addWidget(sessionTable_, 1);

    // ---- 右：消息预览 ----
    preview_ = new QTextBrowser(this);
    preview_->setOpenExternalLinks(true);
    preview_->setPlaceholderText(QStringLiteral("选择一个会话以预览消息..."));

    auto* rightWrap = new QWidget(this);
    auto* rightLay  = new QVBoxLayout(rightWrap);
    rightLay->setContentsMargins(0, 0, 0, 0);
    rightLay->setSpacing(4);
    auto* rightHdr = new QLabel(QStringLiteral("消息预览"), rightWrap);
    rightHdr->setObjectName(QStringLiteral("sectionHeader"));
    rightLay->addWidget(rightHdr);
    rightLay->addWidget(preview_, 1);

    // ---- 三栏 splitter ----
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(leftWrap);
    split->addWidget(midWrap);
    split->addWidget(rightWrap);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 2);
    split->setHandleWidth(1);

    // ---- 底部按钮条 ----
    hint_ = new QLabel(QStringLiteral("提示：会话按 SHA256 隔离；EXE 被自动更新后旧会话会进入此处。"), this);
    hint_->setObjectName(QStringLiteral("sectionHeader"));

    importBtn_ = new QPushButton(QStringLiteral("导入到当前项目"), this);
    importBtn_->setEnabled(false);
    importBtn_->setToolTip(QStringLiteral("把所选会话的全部 messages 拷贝到当前活动项目，作为新会话"));

    closeBtn_  = new QPushButton(QStringLiteral("关闭"), this);

    auto* btnBar = new QHBoxLayout();
    btnBar->setContentsMargins(8, 4, 8, 4);
    btnBar->addWidget(hint_, 1);
    btnBar->addWidget(importBtn_);
    btnBar->addWidget(closeBtn_);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);
    root->addWidget(split, 1);
    root->addLayout(btnBar);

    connect(dbList_, &QListWidget::currentRowChanged,
            this, &HistoryDialog::onDbSelected);
    connect(sessionTable_, &QTableWidget::cellClicked,
            this, &HistoryDialog::onSessionSelected);
    connect(sessionTable_, &QTableWidget::currentCellChanged,
            this, [this](int r, int c, int, int) { onSessionSelected(r, c); });
    connect(importBtn_, &QPushButton::clicked,
            this, &HistoryDialog::onImportClicked);
    connect(closeBtn_, &QPushButton::clicked,
            this, &QDialog::accept);

    reloadDbList();
}

void HistoryDialog::reloadDbList() {
    dbList_->clear();
    dbs_ = ProjectBrowser::listProjectDbs();

    const std::string activeSha = ProjectContext::instance().projectId();

    int activeRow = -1;
    for (size_t i = 0; i < dbs_.size(); ++i) {
        const auto& d = dbs_[i];
        QString sha12 = QString::fromStdString(d.sha256Hex.substr(0, 12));

        // K-01：有 EXE 文件名时优先显示文件名 + SHA 前 8；否则回退到纯 SHA12。
        // 第二行展示大小 / mtime，让信息密度更高。
        QString head;
        if (!d.exeFilename.empty()) {
            head = QStringLiteral("%1  ·  %2")
                       .arg(QString::fromStdString(d.exeFilename),
                            QString::fromStdString(d.sha256Hex.substr(0, 8)));
        } else {
            head = sha12;
        }
        QString line = QStringLiteral("%1\n    %2  ·  %3")
                           .arg(head, formatSize(d.sizeBytes), formatEpoch(d.mtimeEpoch));

        bool isActive = (d.sha256Hex == activeSha);
        if (isActive) {
            line.prepend(QStringLiteral("● "));   // 当前项目标记
            activeRow = static_cast<int>(i);
        } else {
            line.prepend(QStringLiteral("  "));
        }
        auto* item = new QListWidgetItem(line, dbList_);

        // tooltip：完整信息（含 EXE 完整路径 + SHA + first/last seen）
        QString tip;
        if (!d.exePath.empty()) {
            tip += QStringLiteral("路径：%1\n").arg(QString::fromStdString(d.exePath));
        }
        tip += QStringLiteral("SHA256：%1\n").arg(QString::fromStdString(d.sha256Hex));
        tip += QStringLiteral("文件：%1   大小：%2\n")
                   .arg(QString::fromStdString(d.path.string()), formatSize(d.sizeBytes));
        if (d.firstSeenEpoch > 0) {
            tip += QStringLiteral("首次：%1\n").arg(formatEpoch(d.firstSeenEpoch));
        }
        if (d.lastSeenEpoch > 0) {
            tip += QStringLiteral("最近：%1").arg(formatEpoch(d.lastSeenEpoch));
        }
        item->setToolTip(tip);

        if (isActive) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
    }

    if (dbs_.empty()) {
        auto* item = new QListWidgetItem(QStringLiteral("（无历史项目库）"), dbList_);
        item->setFlags(Qt::NoItemFlags);
    } else {
        // 默认选中：优先当前项目；否则第一项
        dbList_->setCurrentRow(activeRow >= 0 ? activeRow : 0);
    }
}

void HistoryDialog::onDbSelected(int row) {
    currentDbRow_ = row;
    sessionTable_->setRowCount(0);
    preview_->clear();
    importBtn_->setEnabled(false);
    currentSessionId_ = 0;
    if (row < 0 || row >= static_cast<int>(dbs_.size())) return;
    reloadSessionTable(dbs_[row].path);
}

void HistoryDialog::reloadSessionTable(const std::filesystem::path& dbPath) {
    auto sessions = ProjectBrowser::listSessions(dbPath);
    sessionTable_->setRowCount(static_cast<int>(sessions.size()));
    for (int i = 0; i < static_cast<int>(sessions.size()); ++i) {
        const auto& s = sessions[i];
        auto* idItem = new QTableWidgetItem(QString::number(s.id));
        idItem->setData(Qt::UserRole, static_cast<qlonglong>(s.id));
        idItem->setData(Qt::UserRole + 1, QString::fromStdString(s.title));
        idItem->setData(Qt::UserRole + 2, QString::fromStdString(s.model));
        sessionTable_->setItem(i, 0, idItem);
        sessionTable_->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(s.title)));
        sessionTable_->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(s.model)));
        sessionTable_->setItem(i, 3, new QTableWidgetItem(formatEpoch(s.updatedAt)));
    }
    if (sessions.empty()) {
        // 留空，由调用方提示
        preview_->setPlainText(QStringLiteral("(此 db 中没有会话)"));
    }
}

void HistoryDialog::onSessionSelected(int row, int /*col*/) {
    importBtn_->setEnabled(false);
    currentSessionId_ = 0;
    if (row < 0 || currentDbRow_ < 0) { preview_->clear(); return; }
    auto* idItem = sessionTable_->item(row, 0);
    if (!idItem) { preview_->clear(); return; }
    currentSessionId_     = static_cast<std::int64_t>(idItem->data(Qt::UserRole).toLongLong());
    currentSessionTitle_  = idItem->data(Qt::UserRole + 1).toString().toStdString();
    currentSessionModel_  = idItem->data(Qt::UserRole + 2).toString().toStdString();

    reloadMessagePreview(dbs_[currentDbRow_].path, currentSessionId_);

    // 仅当当前 db != 活动 db 时允许导入；同库导入意义不大
    bool sameAsActive = (dbs_[currentDbRow_].sha256Hex
                         == ProjectContext::instance().projectId());
    bool hasActive    = ProjectContext::instance().store() != nullptr;
    importBtn_->setEnabled(currentSessionId_ > 0 && hasActive && !sameAsActive);
    if (sameAsActive) {
        importBtn_->setToolTip(QStringLiteral("已是当前项目，无需导入"));
    } else if (!hasActive) {
        importBtn_->setToolTip(QStringLiteral("当前未在调试任何程序，无目标项目可导入"));
    } else {
        importBtn_->setToolTip(QStringLiteral("把所选会话的全部 messages 拷贝到当前活动项目"));
    }
}

void HistoryDialog::reloadMessagePreview(const std::filesystem::path& dbPath,
                                        std::int64_t sessionId) {
    auto msgs = ProjectBrowser::listMessages(dbPath, sessionId);
    QString html;
    html.reserve(static_cast<int>(msgs.size()) * 256);
    html.append(QStringLiteral("<style>"
        ".role{color:#9cdcfe;font-weight:600;margin-top:8px;}"
        ".sys{color:#a0a0a0;font-style:italic;}"
        "pre{white-space:pre-wrap;background:#1e1e1e;color:#d4d4d4;"
            "padding:6px;border:1px solid #333;}"
        "</style>"));
    for (const auto& m : msgs) {
        QString role = QString::fromStdString(m.role);
        QString cls  = (m.role == "system") ? QStringLiteral("sys") : QStringLiteral("role");
        html.append(QStringLiteral("<div class='%1'>[%2]</div>").arg(cls, role));
        // 转义内容；不渲染 markdown，避免触发外部资源/嵌套表格
        QString content = QString::fromStdString(m.content)
                             .toHtmlEscaped();
        html.append(QStringLiteral("<pre>%1</pre>").arg(content));
    }
    if (msgs.empty()) {
        html = QStringLiteral("<i>(此会话无消息)</i>");
    }
    preview_->setHtml(html);
}

void HistoryDialog::onImportClicked() {
    if (currentDbRow_ < 0 || currentSessionId_ <= 0) return;
    auto store = ProjectContext::instance().store();
    if (!store) {
        QMessageBox::warning(this, QStringLiteral("无法导入"),
            QStringLiteral("当前没有活动项目（尚未开始调试任何程序）。"));
        return;
    }

    const auto& srcDb = dbs_[currentDbRow_].path;
    auto msgs = ProjectBrowser::listMessages(srcDb, currentSessionId_);
    if (msgs.empty()) {
        QMessageBox::information(this, QStringLiteral("空会话"),
            QStringLiteral("此会话没有可导入的消息。"));
        return;
    }

    QString srcSha12 = QString::fromStdString(dbs_[currentDbRow_].sha256Hex.substr(0, 12));
    QString origTitle = QString::fromStdString(currentSessionTitle_);
    if (origTitle.isEmpty()) origTitle = QStringLiteral("会话 #%1").arg(currentSessionId_);
    QString newTitle = QStringLiteral("%1（导入自 %2）").arg(origTitle, srcSha12);

    int64_t newId = store->createSession(newTitle.toStdString(), currentSessionModel_);
    if (newId <= 0) {
        QMessageBox::critical(this, QStringLiteral("导入失败"),
            QStringLiteral("无法在当前项目库中创建会话。"));
        return;
    }
    int copied = 0;
    for (const auto& m : msgs) {
        // K-41c: 复制会话时带上 tool_call_id / tool_name / tool_calls 三列，
        // 否则跨项目导入后续跑 agent 会因 tool 消息缺 tool_call_id 配对而报 400。
        // 老 source 库（没新列）拿到的 MessageRow 三字段已是空默认值，等价于老行为。
        if (store->appendMessageEx(newId, m.role, m.content,
                                   m.toolCallId, m.toolName, m.toolCalls) > 0) {
            ++copied;
        }
    }
    XAI_LOG_INFO("imported {} msgs from {} session {} -> active session {}",
                 copied, srcDb.string(), currentSessionId_, newId);

    QMessageBox::information(this, QStringLiteral("导入完成"),
        QStringLiteral("已导入 %1 条消息到新会话「%2」。").arg(copied).arg(newTitle));

    emit imported();
    // 不关闭对话框，方便继续浏览/导入下一会话
}

}  // namespace x64ai
