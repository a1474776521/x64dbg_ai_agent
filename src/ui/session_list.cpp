// ui/session_list.cpp
#include "ui/session_list.h"

#include <QAbstractItemDelegate>
#include <QAction>
#include <QDateTime>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include "storage/project_context.h"
#include "storage/session_store.h"
#include "util/logging.h"

namespace x64ai {

namespace {

constexpr int kTitleRole = Qt::UserRole + 1;
constexpr int kTimeRole  = Qt::UserRole + 2;

QString formatTime(qint64 epoch) {
    return QDateTime::fromSecsSinceEpoch(epoch).toString("MM-dd HH:mm");
}

// 双行 delegate：标题（醒目）+ 时间（次要）
class SessionItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&,
                   const QModelIndex&) const override {
        return QSize(0, 44);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& idx) const override {
        p->save();
        const bool selected = (opt.state & QStyle::State_Selected);
        const bool hovered  = (opt.state & QStyle::State_MouseOver);

        QRect r = opt.rect.adjusted(2, 1, -2, -1);
        QColor bg(Qt::transparent);
        if (selected)      bg = QColor("#094771");
        else if (hovered)  bg = QColor("#2a2d2e");
        if (bg.alpha() != 0) {
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(Qt::NoPen);
            p->setBrush(bg);
            p->drawRoundedRect(r, 4, 4);
        }

        const QString title = idx.data(kTitleRole).toString();
        const QString time  = idx.data(kTimeRole).toString();

        QRect inner = r.adjusted(10, 4, -10, -4);
        const QColor titleCol = selected ? QColor("#ffffff") : QColor("#d4d4d4");
        const QColor timeCol  = selected ? QColor("#cfe6ff") : QColor("#7a7a7a");

        QFont f = opt.font;
        f.setPointSizeF(f.pointSizeF() + 0.5);
        f.setBold(true);
        p->setFont(f);
        p->setPen(titleCol);
        QFontMetrics fm(f);
        QString elided = fm.elidedText(title, Qt::ElideRight, inner.width());
        p->drawText(inner.adjusted(0, 0, 0, -inner.height() / 2),
                    Qt::AlignLeft | Qt::AlignVCenter, elided);

        QFont f2 = opt.font;
        f2.setPointSizeF(qMax(7.5, f2.pointSizeF() - 1.0));
        p->setFont(f2);
        p->setPen(timeCol);
        p->drawText(inner.adjusted(0, inner.height() / 2, 0, 0),
                    Qt::AlignLeft | Qt::AlignVCenter, time);

        p->restore();
    }
};

}  // namespace

SessionListWidget::SessionListWidget(QWidget* parent) : QWidget(parent)
{
    header_ = new QLabel(QStringLiteral("会话（未连接）"), this);
    header_->setObjectName(QStringLiteral("sectionHeader"));

    newBtn_ = new QPushButton(QStringLiteral(" 新建"), this);
    newBtn_->setObjectName(QStringLiteral("secondaryBtn"));
    newBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/plus.svg")));
    newBtn_->setCursor(Qt::PointingHandCursor);
    newBtn_->setToolTip(QStringLiteral("为当前被调试程序新建一个会话"));

    list_ = new QListWidget(this);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setMouseTracking(true);
    list_->setFrameShape(QFrame::NoFrame);
    list_->setItemDelegate(new SessionItemDelegate(list_));
    list_->setUniformItemSizes(true);

    auto* top = new QHBoxLayout();
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(6);
    top->addWidget(header_, 1);
    top->addWidget(newBtn_);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);
    root->addLayout(top);
    root->addWidget(list_, 1);

    connect(newBtn_, &QPushButton::clicked,
            this, &SessionListWidget::onCreateClicked);
    connect(list_, &QListWidget::itemActivated,
            this, &SessionListWidget::onItemActivated);
    connect(list_, &QListWidget::itemClicked,
            this, &SessionListWidget::onItemActivated);
    connect(list_, &QListWidget::customContextMenuRequested,
            this, &SessionListWidget::onContextMenu);
}

void SessionListWidget::refresh()
{
    list_->clear();
    auto store = ProjectContext::instance().store();
    if (!store) {
        header_->setText(QStringLiteral("会话（未在调试）"));
        newBtn_->setEnabled(false);
        currentId_ = 0;
        return;
    }
    newBtn_->setEnabled(true);
    auto pid = ProjectContext::instance().projectId();
    header_->setText(QStringLiteral("会话  ·  %1…")
                         .arg(QString::fromStdString(pid.substr(0, 8))));

    auto sessions = store->listSessions();
    int currentRow = -1;
    int row = 0;
    for (const auto& s : sessions) {
        auto* item = new QListWidgetItem(list_);
        item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(s.id));
        item->setData(kTitleRole, QString::fromStdString(s.title));
        item->setData(kTimeRole,  formatTime(s.updatedAt));
        if (s.id == currentId_) currentRow = row;
        ++row;
    }
    if (currentRow >= 0) list_->setCurrentRow(currentRow);
}

void SessionListWidget::onCreateClicked()
{
    auto store = ProjectContext::instance().store();
    if (!store) return;

    bool ok = false;
    QString title = QInputDialog::getText(
        this, QStringLiteral("新建会话"), QStringLiteral("会话标题："),
        QLineEdit::Normal, QStringLiteral("新会话"), &ok);
    if (!ok || title.trimmed().isEmpty()) return;

    int64_t id = store->createSession(title.toStdString(), "");
    if (id <= 0) {
        QMessageBox::warning(this, QStringLiteral("失败"),
                             QStringLiteral("创建会话失败，详见日志。"));
        return;
    }
    currentId_ = id;
    emit sessionsChanged();
    refresh();
    emit sessionSelected(static_cast<qint64>(id));
}

void SessionListWidget::onItemActivated(QListWidgetItem* item)
{
    if (!item) return;
    int64_t id = item->data(Qt::UserRole).toLongLong();
    if (id <= 0 || id == currentId_) return;
    currentId_ = id;
    emit sessionSelected(static_cast<qint64>(id));
}

void SessionListWidget::onContextMenu(const QPoint& pos)
{
    auto* item = list_->itemAt(pos);
    if (!item) return;
    int64_t id = item->data(Qt::UserRole).toLongLong();
    if (id <= 0) return;

    QMenu menu(this);
    auto* actRename = menu.addAction(QStringLiteral("重命名"));
    auto* actDelete = menu.addAction(
        QIcon(QStringLiteral(":/x64dbg-ai/icons/trash.svg")),
        QStringLiteral("删除"));
    auto* chosen = menu.exec(list_->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    auto store = ProjectContext::instance().store();
    if (!store) return;

    if (chosen == actRename) {
        bool ok = false;
        const QString oldTitle = item->data(kTitleRole).toString();
        QString title = QInputDialog::getText(
            this, QStringLiteral("重命名"), QStringLiteral("新标题："),
            QLineEdit::Normal, oldTitle, &ok);
        if (ok && !title.trimmed().isEmpty()) {
            store->renameSession(id, title.toStdString());
            emit sessionsChanged();
            refresh();
        }
    } else if (chosen == actDelete) {
        if (QMessageBox::question(this, QStringLiteral("确认"),
                QStringLiteral("确定删除该会话及其全部消息？"))
            == QMessageBox::Yes) {
            store->deleteSession(id);
            if (currentId_ == id) currentId_ = 0;
            emit sessionsChanged();
            refresh();
        }
    }
}

}  // namespace x64ai
