// ui/locator_dialog.cpp
#include "ui/locator_dialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include "_plugins.h"
#include "ai/copilot_chat_client.h"
#include "ai/embedding_client.h"
#include "debugger/disasm_context.h"
#include "locator/locator_engine.h"
#include "ui/assistant_panel.h"
#include "util/logging.h"

#include <map>

namespace x64ai {

namespace {

QString fmtVa(uint64_t va)
{
#ifdef _WIN64
    return QStringLiteral("0x%1").arg(va, 16, 16, QLatin1Char('0')).toUpper();
#else
    return QStringLiteral("0x%1").arg(static_cast<quint32>(va), 8, 16, QLatin1Char('0')).toUpper();
#endif
}

QColor scoreColor(int s)
{
    if (s >= 90) return QColor("#f48771");   // 红：高度可疑
    if (s >= 75) return QColor("#dcdcaa");   // 黄
    if (s >= 50) return QColor("#9cdcfe");   // 蓝
    return QColor("#9da5b4");                // 灰
}

}  // namespace

LocatorDialog::LocatorDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("启发式定位器"));
    resize(880, 560);

    cbApi_      = new QCheckBox(QStringLiteral("API 扫描"), this);
    cbString_   = new QCheckBox(QStringLiteral("字符串扫描"), this);
    cbPattern_  = new QCheckBox(QStringLiteral("特征码扫描"), this);
    cbWriteRag_ = new QCheckBox(QStringLiteral("高分自动写 RAG"), this);
    cbApi_->setChecked(true);
    cbString_->setChecked(true);
    cbPattern_->setChecked(true);
    cbWriteRag_->setChecked(true);

    scanBtn_   = new QPushButton(QStringLiteral("开始扫描"), this);
    statusLbl_ = new QLabel(QStringLiteral("就绪"), this);
    statusLbl_->setObjectName(QStringLiteral("statusBarLabel"));

    kwEdit_ = new QLineEdit(this);
    kwEdit_->setPlaceholderText(QStringLiteral(
        "关键字（可选）：逗号或空格分隔；hex 模式如 \"DE AD BE EF\" 或 \"48 8B ?? E8\""));
    kwEdit_->setClearButtonEnabled(true);
    {
        QSettings st(QStringLiteral("x64dbg-ai-plugin"), QStringLiteral("locator"));
        kwEdit_->setText(st.value(QStringLiteral("userKeywords")).toString());
    }

    auto* top = new QHBoxLayout();
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(8);
    top->addWidget(cbApi_);
    top->addWidget(cbString_);
    top->addWidget(cbPattern_);
    top->addSpacing(12);
    top->addWidget(cbWriteRag_);
    top->addStretch(1);
    top->addWidget(statusLbl_);
    top->addWidget(scanBtn_);

    auto* kwRow = new QHBoxLayout();
    kwRow->setContentsMargins(0, 0, 0, 0);
    kwRow->setSpacing(8);
    auto* kwLbl = new QLabel(QStringLiteral("关键字:"), this);
    kwRow->addWidget(kwLbl);
    kwRow->addWidget(kwEdit_, 1);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(4);
    tree_->setHeaderLabels({QStringLiteral("地址 / 分组"),
                            QStringLiteral("标签"),
                            QStringLiteral("分数"),
                            QStringLiteral("证据")});
    tree_->setRootIsDecorated(true);
    tree_->setAlternatingRowColors(false);
    tree_->setUniformRowHeights(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_->header()->resizeSection(0, 220);
    tree_->header()->resizeSection(1, 320);
    tree_->header()->resizeSection(2, 60);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);
    root->addLayout(top);
    root->addLayout(kwRow);
    root->addWidget(tree_, 1);

    connect(scanBtn_, &QPushButton::clicked, this, &LocatorDialog::onScanClicked);
    connect(tree_, &QTreeWidget::itemDoubleClicked,
            this, &LocatorDialog::onItemDoubleClicked);
    connect(tree_, &QTreeWidget::customContextMenuRequested,
            this, &LocatorDialog::onContextMenu);
}

void LocatorDialog::onScanClicked()
{
    if (!DbgIsDebugging()) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("当前未在调试，无法扫描。"));
        return;
    }

    LocatorEngine::Options opt;
    opt.runApi      = cbApi_->isChecked();
    opt.runString   = cbString_->isChecked();
    opt.runPattern  = cbPattern_->isChecked();
    opt.ragMinScore = cbWriteRag_->isChecked() ? 60 : 0;

    // 解析关键字：以逗号/分号/制表符分隔；空格分隔仅在非 hex 形态下生效
    // —— 为支持 "DE AD BE EF" 这种空格 hex，我们用 "," / ";" / 换行 作为主分隔，
    //    再对每段 trim；若用户想输入多个普通字符串，请用逗号分隔。
    const QString kwLine = kwEdit_->text();
    {
        QSettings st(QStringLiteral("x64dbg-ai-plugin"), QStringLiteral("locator"));
        st.setValue(QStringLiteral("userKeywords"), kwLine);
    }
    QStringList tokens = kwLine.split(QRegularExpression(QStringLiteral("[,;\\n]")),
                                      QString::SkipEmptyParts);
    for (auto& t : tokens) t = t.trimmed();
    tokens.removeAll(QString());
    for (const auto& t : tokens) {
        if (!t.isEmpty()) opt.userKeywords.push_back(t.toStdString());
    }

    if (!opt.runApi && !opt.runString && !opt.runPattern) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("请至少选择一个扫描器。"));
        return;
    }

    scanBtn_->setEnabled(false);
    tree_->clear();
    statusLbl_->setText(QStringLiteral("扫描中..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);

    auto* self = this;
    QtConcurrent::run([self, opt]() {
        auto progress = [self](const std::string& phase, int cur, int tot, bool done) {
            QString msg = QStringLiteral("[%1] %2/%3 %4")
                              .arg(QString::fromStdString(phase))
                              .arg(cur).arg(tot)
                              .arg(done ? QStringLiteral("✓") : QStringLiteral("…"));
            QMetaObject::invokeMethod(self, [self, msg]() {
                self->statusLbl_->setText(msg);
            }, Qt::QueuedConnection);
        };
        auto hits = LocatorEngine::runAll(opt, progress);

        QMetaObject::invokeMethod(self, [self, hits]() {
            QApplication::restoreOverrideCursor();
            self->scanBtn_->setEnabled(true);
            self->populateResults(hits);
            self->statusLbl_->setText(
                QStringLiteral("完成，共 %1 条").arg(hits.size()));
        }, Qt::QueuedConnection);
    });
}

void LocatorDialog::populateResults(const std::vector<HeuristicHit>& hits)
{
    tree_->clear();
    if (hits.empty()) return;

    // 先按 kind 分顶层分组，再按 category 二级分组
    std::map<std::string, QTreeWidgetItem*> kindGroup;
    std::map<std::pair<std::string, std::string>, QTreeWidgetItem*> catGroup;

    auto kindLabel = [](HitKind k) -> QString {
        switch (k) {
            case HitKind::Api:     return QStringLiteral("API 命中");
            case HitKind::String:  return QStringLiteral("字符串命中");
            case HitKind::Pattern: return QStringLiteral("特征码命中");
        }
        return QStringLiteral("?");
    };

    int totalShown = 0;
    for (const auto& h : hits) {
        std::string kk = hitKindName(h.kind);
        if (!kindGroup.count(kk)) {
            auto* g = new QTreeWidgetItem(tree_);
            g->setText(0, kindLabel(h.kind));
            g->setFirstColumnSpanned(false);
            g->setExpanded(true);
            QFont f = g->font(0); f.setBold(true);
            g->setFont(0, f);
            kindGroup[kk] = g;
        }
        auto& parent = kindGroup[kk];

        auto catKey = std::make_pair(kk, h.category);
        if (!catGroup.count(catKey)) {
            auto* c = new QTreeWidgetItem(parent);
            c->setText(0, QString::fromStdString(h.category));
            c->setExpanded(true);
            QFont f = c->font(0); f.setItalic(true);
            c->setFont(0, f);
            c->setForeground(0, QColor("#9da5b4"));
            catGroup[catKey] = c;
        }
        auto* parentCat = catGroup[catKey];

        auto* item = new QTreeWidgetItem(parentCat);
        item->setText(0, fmtVa(h.va));
        item->setText(1, QString::fromStdString(h.label));
        item->setText(2, QString::number(h.score));
        item->setText(3, QString::fromStdString(h.evidence));
        item->setForeground(2, scoreColor(h.score));
        item->setData(0, Qt::UserRole,     QVariant::fromValue<qulonglong>(h.va));
        item->setData(0, Qt::UserRole + 1, QVariant::fromValue<qulonglong>(h.refVa));
        item->setData(0, Qt::UserRole + 2, QString::fromStdString(h.label));
        item->setData(0, Qt::UserRole + 3, static_cast<int>(h.kind));
        ++totalShown;
    }

    for (auto& [k, g] : kindGroup) {
        int n = g->childCount();
        int leaves = 0;
        for (int i = 0; i < n; ++i) leaves += g->child(i)->childCount();
        g->setText(0, g->text(0) + QStringLiteral("  (%1)").arg(leaves));
    }

    XAI_LOG_INFO("LocatorDialog: populated {} items", totalShown);
}

void LocatorDialog::onItemDoubleClicked(QTreeWidgetItem* item, int /*col*/)
{
    if (!item) return;
    QVariant v = item->data(0, Qt::UserRole);
    if (!v.isValid()) return;
    qulonglong va = v.toULongLong();
    if (va == 0) return;

    char cmd[64];
    std::snprintf(cmd, sizeof(cmd), "disasm 0x%llX", static_cast<unsigned long long>(va));
    DbgCmdExec(cmd);
}

void LocatorDialog::onContextMenu(const QPoint& pos)
{
    auto* item = tree_->itemAt(pos);
    if (!item) return;
    QVariant v = item->data(0, Qt::UserRole);
    if (!v.isValid()) return;
    qulonglong va = v.toULongLong();
    if (va == 0) return;

    QMenu menu(this);
    auto* actGoto = menu.addAction(QStringLiteral("跳转到此地址"));
    auto* actAi   = menu.addAction(QStringLiteral("让 AI 分析此处"));
    auto* chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == actGoto) {
        char cmd[64];
        std::snprintf(cmd, sizeof(cmd), "disasm 0x%llX",
                      static_cast<unsigned long long>(va));
        DbgCmdExec(cmd);
        return;
    }

    if (chosen == actAi) {
        // 先跳转再走 analyzeCurrentAddress（它会取当前选中地址）
        char cmd[64];
        std::snprintf(cmd, sizeof(cmd), "disasm 0x%llX",
                      static_cast<unsigned long long>(va));
        DbgCmdExec(cmd);
        // 给 x64dbg 一点时间刷新选中点（同步即可，DbgCmdExec 是异步派发但 disasm 通常立即生效）
        AssistantPanel::analyzeCurrentAddress();
    }
}

}  // namespace x64ai
