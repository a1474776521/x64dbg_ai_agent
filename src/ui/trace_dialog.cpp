// ui/trace_dialog.cpp
#include "ui/trace_dialog.h"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextEdit>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>

#include "_plugins.h"
#include "ai/copilot_chat_client.h"
#include "trace/callstack_tracer.h"
#include "trace/trace_recorder.h"
#include "ui/assistant_panel.h"
#include "util/logging.h"

namespace x64ai {

namespace {

QString fmtVa(uint64_t va)
{
#ifdef _WIN64
    return QStringLiteral("0x%1").arg(va, 16, 16, QLatin1Char('0')).toUpper();
#else
    return QStringLiteral("0x%1")
        .arg(static_cast<quint32>(va), 8, 16, QLatin1Char('0')).toUpper();
#endif
}

constexpr int kRoleAddr = Qt::UserRole;
constexpr int kRoleSym  = Qt::UserRole + 1;
constexpr int kRoleHits = Qt::UserRole + 2;

// ComboBox 索引
constexpr int kModeTargeted       = 0;
constexpr int kModeGlobalActive   = 1;
constexpr int kModeGlobalPassive  = 2;
constexpr int kModeCallStack      = 3;

QString resolveLabel(uint64_t va)
{
    if (va == 0) return {};
    char modBuf[MAX_MODULE_SIZE]  = {0};
    char labBuf[MAX_LABEL_SIZE]   = {0};
    bool hasMod = DbgGetModuleAt(static_cast<duint>(va), modBuf);
    bool hasLab = DbgGetLabelAt(static_cast<duint>(va), SEG_DEFAULT, labBuf);
    if (hasMod && hasLab && labBuf[0])
        return QString::fromUtf8(modBuf) + "." + QString::fromUtf8(labBuf);
    if (hasLab && labBuf[0])
        return QString::fromUtf8(labBuf);
    if (hasMod && modBuf[0])
        return QString::fromUtf8(modBuf);
    return {};
}

}  // namespace

TraceDialog::TraceDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("调用链追溯"));
    resize(1080, 720);

    // ---- 顶部：模式 + 地址输入 ----
    modeBox_ = new QComboBox(this);
    modeBox_->addItem(QStringLiteral("目标函数追溯（推荐）"));
    modeBox_->addItem(QStringLiteral("全局录制 - 主动启动"));
    modeBox_->addItem(QStringLiteral("全局录制 - 被动监听"));
    modeBox_->addItem(QStringLiteral("调用栈采样（推荐用于 send/系统 API 反查）"));
    modeBox_->setCurrentIndex(kModeTargeted);
    modeBox_->setMinimumWidth(220);

    addrEdit_ = new QLineEdit(this);
    addrEdit_->setPlaceholderText(
        QStringLiteral("函数地址 / 名称（如 0x401000、main、kernel32.CreateFileW）"));

    resolveBtn_ = new QToolButton(this);
    resolveBtn_->setText(QStringLiteral(" 解析"));
    resolveBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/refresh.svg")));
    resolveBtn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    resolveBtn_->setToolTip(QStringLiteral("把输入解析为函数 [start,end)"));

    maxSamplesLbl_ = new QLabel(QStringLiteral("最大采样："), this);
    maxSamplesBox_ = new QSpinBox(this);
    maxSamplesBox_->setRange(1, 4096);
    maxSamplesBox_->setValue(32);
    maxSamplesBox_->setToolTip(QStringLiteral(
        "调用栈采样模式下，命中达到此次数后自动拆断点并 run"));

    auto* row1 = new QHBoxLayout();
    row1->setContentsMargins(0, 0, 0, 0);
    row1->setSpacing(6);
    row1->addWidget(new QLabel(QStringLiteral("模式："), this));
    row1->addWidget(modeBox_);
    row1->addSpacing(8);
    row1->addWidget(new QLabel(QStringLiteral("目标："), this));
    row1->addWidget(addrEdit_, 1);
    row1->addWidget(resolveBtn_);
    row1->addSpacing(8);
    row1->addWidget(maxSamplesLbl_);
    row1->addWidget(maxSamplesBox_);

    targetLbl_ = new QLabel(QStringLiteral("当前目标：（未解析）"), this);
    targetLbl_->setObjectName(QStringLiteral("statusBarLabel"));
    targetLbl_->setWordWrap(true);

    // ---- 控制条 ----
    btnStart_ = new QToolButton(this);
    btnStart_->setText(QStringLiteral(" 启动"));
    btnStart_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/git-branch.svg")));
    btnStart_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    btnStop_ = new QToolButton(this);
    btnStop_->setText(QStringLiteral(" 停止"));
    btnStop_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/logout.svg")));
    btnStop_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    btnClear_ = new QToolButton(this);
    btnClear_->setText(QStringLiteral(" 清空"));
    btnClear_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/trash.svg")));
    btnClear_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    btnRefresh_ = new QToolButton(this);
    btnRefresh_->setText(QStringLiteral(" 刷新树"));
    btnRefresh_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/refresh.svg")));
    btnRefresh_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    chkHideSys_ = new QCheckBox(QStringLiteral("隐藏系统模块"), this);
    chkHideSys_->setChecked(true);
    chkHideSys_->setToolTip(QStringLiteral(
        "折叠 ucrtbase / kernel32 / ntdll / user32 等系统 DLL 子树，"
        "避免 CRT 噪声淹没业务调用链"));

    chkMerge_ = new QCheckBox(QStringLiteral("合并相同子调用"), this);
    chkMerge_->setChecked(true);
    chkMerge_->setToolTip(QStringLiteral(
        "同一父节点下重复出现的子函数合并为一行，hits 累加"));

    statusLbl_ = new QLabel(QStringLiteral("就绪"), this);
    statusLbl_->setObjectName(QStringLiteral("statusBarLabel"));

    auto* row2 = new QHBoxLayout();
    row2->setContentsMargins(0, 0, 0, 0);
    row2->setSpacing(6);
    row2->addWidget(btnStart_);
    row2->addWidget(btnStop_);
    row2->addSpacing(12);
    row2->addWidget(btnClear_);
    row2->addWidget(btnRefresh_);
    row2->addSpacing(12);
    row2->addWidget(chkHideSys_);
    row2->addWidget(chkMerge_);
    row2->addStretch(1);
    row2->addWidget(statusLbl_);

    // ---- 主体 ----
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(4);
    tree_->setHeaderLabels({QStringLiteral("调用 (callee)"),
                            QStringLiteral("地址"),
                            QStringLiteral("hits"),
                            QStringLiteral("seq")});
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_->header()->resizeSection(0, 420);
    tree_->header()->resizeSection(1, 180);
    tree_->header()->resizeSection(2, 60);

    detail_ = new QTextEdit(this);
    detail_->setReadOnly(true);
    detail_->setPlaceholderText(QStringLiteral("选中节点查看详情"));

    stackTree_ = new QTreeWidget(this);
    stackTree_->setColumnCount(4);
    stackTree_->setHeaderLabels({QStringLiteral("调用者 (caller)"),
                                 QStringLiteral("地址"),
                                 QStringLiteral("hits"),
                                 QStringLiteral("seq")});
    stackTree_->setRootIsDecorated(true);
    stackTree_->setUniformRowHeights(true);
    stackTree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    stackTree_->header()->resizeSection(0, 460);
    stackTree_->header()->resizeSection(1, 180);
    stackTree_->header()->resizeSection(2, 60);

    stack_ = new QStackedWidget(this);
    stack_->addWidget(tree_);       // index 0
    stack_->addWidget(stackTree_);  // index 1

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(stack_);
    split->addWidget(detail_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    // ---- 底部 ----
    aiBtn_ = new QPushButton(QStringLiteral("让 AI 分析此调用链"), this);
    aiBtn_->setEnabled(false);

    auto* bot = new QHBoxLayout();
    bot->setContentsMargins(0, 0, 0, 0);
    bot->addStretch(1);
    bot->addWidget(aiBtn_);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);
    root->addLayout(row1);
    root->addWidget(targetLbl_);
    root->addLayout(row2);
    root->addWidget(split, 1);
    root->addLayout(bot);

    // ---- 信号 ----
    connect(modeBox_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &TraceDialog::onModeChanged);
    connect(resolveBtn_, &QToolButton::clicked, this, &TraceDialog::onResolveClicked);
    connect(addrEdit_,   &QLineEdit::returnPressed, this, &TraceDialog::onResolveClicked);
    connect(btnStart_,   &QToolButton::clicked, this, &TraceDialog::onStartClicked);
    connect(btnStop_,    &QToolButton::clicked, this, &TraceDialog::onStopClicked);
    connect(btnClear_,   &QToolButton::clicked, this, &TraceDialog::onClearClicked);
    connect(btnRefresh_, &QToolButton::clicked, this, &TraceDialog::onRefreshClicked);
    connect(chkHideSys_, &QCheckBox::toggled, this, [this](bool){ rebuildTree(); });
    connect(chkMerge_,   &QCheckBox::toggled, this, [this](bool){ rebuildTree(); });
    connect(tree_, &QTreeWidget::itemClicked,        this, &TraceDialog::onItemClicked);
    connect(tree_, &QTreeWidget::itemDoubleClicked,  this, &TraceDialog::onItemDoubleClicked);
    connect(tree_, &QTreeWidget::customContextMenuRequested,
            this, &TraceDialog::onContextMenu);
    connect(stackTree_, &QTreeWidget::itemClicked,
            this, [this](QTreeWidgetItem* it, int){
                if (!it) { aiBtn_->setEnabled(false); return; }
                detail_->setPlainText(buildStackPathText(it));
                aiBtn_->setEnabled(true);
            });
    connect(stackTree_, &QTreeWidget::itemDoubleClicked,
            this, &TraceDialog::onItemDoubleClicked);
    connect(aiBtn_, &QPushButton::clicked, this, &TraceDialog::onAiAnalyze);

    // 注册事件计数通知
    QPointer<TraceDialog> guard(this);
    TraceRecorder::instance().setNotify([guard](std::size_t cnt) {
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, cnt]() {
            if (!guard) return;
            Q_UNUSED(cnt);
            guard->updateStatus();
        }, Qt::QueuedConnection);
    }, /*coalesce=*/64);

    // 注册状态变更通知
    TraceRecorder::instance().setStateNotify([guard]() {
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard]() {
            if (!guard) return;
            guard->updateStatus();
            guard->updateButtons();
            // 录制停止时自动刷新一次树
            if (!TraceRecorder::instance().isRecording()
                && !TraceRecorder::instance().hasArmed()) {
                guard->rebuildTree();
            }
        }, Qt::QueuedConnection);
    });

    // CallStackTracer 通知
    CallStackTracer::instance().setNotify([guard]() {
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard]() {
            if (!guard) return;
            guard->rebuildStackTree();
            guard->updateStatus();
            guard->updateButtons();
        }, Qt::QueuedConnection);
    });

    onModeChanged(modeBox_->currentIndex());
    updateStatus();
    updateButtons();
}

TraceDialog::~TraceDialog()
{
    TraceRecorder::instance().setNotify({}, 64);
    TraceRecorder::instance().setStateNotify({});
    CallStackTracer::instance().setNotify({});
}

// ====== 外部入口 ======

void TraceDialog::openForTarget(uint64_t va, const QString& displayHint)
{
    modeBox_->setCurrentIndex(kModeTargeted);

    QString text = displayHint;
    if (text.isEmpty()) text = fmtVa(va);
    addrEdit_->setText(text);

    // 直接尝试解析为函数
    uint64_t fs = 0, fe = 0;
    if (TraceRecorder::normalizeToFunction(va, &fs, &fe)) {
        currentTarget_.funcStart = fs;
        currentTarget_.funcEnd   = fe;
        currentTarget_.entryHit  = fs;  // 函数入口
        QString lbl = resolveLabel(fs);
        currentTarget_.label = lbl.isEmpty()
            ? fmtVa(fs).toStdString()
            : lbl.toStdString();
        targetResolved_ = true;
    } else {
        // 不在已识别函数内：以 va 自身为 entry，funcEnd=0（返回判定将退化为只看 SP）
        currentTarget_.funcStart = va;
        currentTarget_.funcEnd   = 0;
        currentTarget_.entryHit  = va;
        QString lbl = resolveLabel(va);
        currentTarget_.label = lbl.isEmpty()
            ? fmtVa(va).toStdString()
            : lbl.toStdString();
        targetResolved_ = true;
    }
    updateTargetLabel();
    updateButtons();
}

// ====== 模式 / 解析 ======

void TraceDialog::onModeChanged(int index)
{
    bool targeted  = (index == kModeTargeted);
    bool callstack = (index == kModeCallStack);
    bool wantsAddr = targeted || callstack;
    addrEdit_->setEnabled(wantsAddr);
    resolveBtn_->setEnabled(wantsAddr);
    targetLbl_->setVisible(wantsAddr);
    maxSamplesLbl_->setVisible(callstack);
    maxSamplesBox_->setVisible(callstack);
    stack_->setCurrentIndex(callstack ? 1 : 0);
    updateButtons();
}

bool TraceDialog::isCallStackMode() const
{
    return modeBox_->currentIndex() == kModeCallStack;
}

void TraceDialog::onResolveClicked()
{
    if (!resolveInput()) {
        QMessageBox::warning(this, QStringLiteral("解析失败"),
            QStringLiteral("无法解析地址或函数名：%1\n"
                           "请检查是否处于调试状态，或输入是否正确。")
                .arg(addrEdit_->text()));
    }
    updateTargetLabel();
    updateButtons();
}

bool TraceDialog::resolveInput()
{
    targetResolved_  = false;
    currentTarget_   = {};

    QString in = addrEdit_->text().trimmed();
    if (in.isEmpty()) return false;

    if (!DbgIsDebugging()) return false;

    uint64_t va = TraceRecorder::resolveAddressOrName(in.toStdString());
    if (va == 0) return false;

    uint64_t fs = 0, fe = 0;
    if (TraceRecorder::normalizeToFunction(va, &fs, &fe)) {
        currentTarget_.funcStart = fs;
        currentTarget_.funcEnd   = fe;
        currentTarget_.entryHit  = fs;
    } else {
        currentTarget_.funcStart = va;
        currentTarget_.funcEnd   = 0;
        currentTarget_.entryHit  = va;
    }
    QString lbl = resolveLabel(currentTarget_.entryHit);
    currentTarget_.label = lbl.isEmpty()
        ? fmtVa(currentTarget_.entryHit).toStdString()
        : lbl.toStdString();
    targetResolved_ = true;
    return true;
}

void TraceDialog::updateTargetLabel()
{
    if (!targetResolved_) {
        targetLbl_->setText(QStringLiteral("当前目标：（未解析）"));
        return;
    }
    QString s = QStringLiteral("当前目标：%1   entry=%2")
        .arg(QString::fromStdString(currentTarget_.label))
        .arg(fmtVa(currentTarget_.entryHit));
    if (currentTarget_.funcStart && currentTarget_.funcEnd) {
        s += QStringLiteral("   func=[%1, %2)")
                .arg(fmtVa(currentTarget_.funcStart))
                .arg(fmtVa(currentTarget_.funcEnd));
    } else {
        s += QStringLiteral("   func=（未识别，将仅按 SP 判定返回）");
    }
    targetLbl_->setText(s);
}

// ====== 启动 / 停止 / 清空 / 刷新 ======

void TraceDialog::onStartClicked()
{
    if (!DbgIsDebugging()) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("当前未在调试。"));
        return;
    }
    auto& rec = TraceRecorder::instance();
    int mode = modeBox_->currentIndex();

    if (mode == kModeTargeted) {
        if (!targetResolved_) {
            // 自动解析一次
            if (!resolveInput()) {
                QMessageBox::warning(this, QStringLiteral("提示"),
                    QStringLiteral("请先在地址框输入并解析目标。"));
                return;
            }
            updateTargetLabel();
        }
        if (!rec.startTargeted(currentTarget_)) {
            QMessageBox::warning(this, QStringLiteral("启动失败"),
                QStringLiteral("无法下硬件断点；请检查地址是否在可执行节中。"));
            return;
        }
    } else if (mode == kModeGlobalActive) {
        rec.startGlobalActive();
    } else if (mode == kModeGlobalPassive) {
        rec.startGlobalPassive();
    } else {
        // 调用栈采样
        if (!targetResolved_) {
            if (!resolveInput()) {
                QMessageBox::warning(this, QStringLiteral("提示"),
                    QStringLiteral("请先在地址框输入并解析目标。"));
                return;
            }
            updateTargetLabel();
        }
        uint32_t maxN = static_cast<uint32_t>(maxSamplesBox_->value());
        if (!CallStackTracer::instance().armWith(
                currentTarget_.entryHit, currentTarget_.label, maxN)) {
            QMessageBox::warning(this, QStringLiteral("启动失败"),
                QStringLiteral("无法下软断点；请检查地址是否在可执行节中。"));
            return;
        }
        stack_->setCurrentIndex(1);
    }

    updateStatus();
    updateButtons();
}

void TraceDialog::onStopClicked()
{
    TraceRecorder::instance().stop();
    CallStackTracer::instance().stop();
    if (isCallStackMode()) rebuildStackTree();
    else                   rebuildTree();
    updateStatus();
    updateButtons();
}

void TraceDialog::onClearClicked()
{
    TraceRecorder::instance().clear();
    CallStackTracer::instance().clear();
    root_.reset();
    view_.reset();
    tree_->clear();
    stackTree_->clear();
    detail_->clear();
    aiBtn_->setEnabled(false);
    updateStatus();
}

void TraceDialog::onRefreshClicked()
{
    if (isCallStackMode()) rebuildStackTree();
    else                   rebuildTree();
}

void TraceDialog::rebuildTree()
{
    auto evs = TraceRecorder::instance().snapshot();
    root_ = CallGraph::build(evs);

    BuildViewOptions opts;
    opts.hideSystemModules = chkHideSys_ ? chkHideSys_->isChecked() : true;
    opts.mergeSiblings     = chkMerge_   ? chkMerge_->isChecked()   : true;
    opts.minHits           = 0;
    view_ = CallGraph::buildView(root_.get(), opts);

    tree_->clear();
    if (view_) {
        for (auto& c : view_->children) {
            populateViewRecursive(nullptr, c.get());
        }
    }
    updateStatus();
}

void TraceDialog::rebuildStackTree()
{
    stackTree_->clear();
    auto samples = CallStackTracer::instance().snapshot();
    if (samples.empty()) return;

    // Trie 节点：按 frames 序列从外到内（线程入口 → 被断函数）建树
    struct Node {
        uint64_t addr = 0;
        uint64_t from = 0;
        std::string sym;
        uint32_t hits = 0;            // 经过本节点的样本 hits 累计
        uint64_t firstSeq = 0;
        std::map<uint64_t, std::unique_ptr<Node>> children;  // key = child.from（caller 中的 call 指令）
    };
    Node root;

    for (const auto& s : samples) {
        Node* cur = &root;
        // frames[0] = 最内层；从最外层（线程入口）向最内层走
        for (auto it = s.frames.rbegin(); it != s.frames.rend(); ++it) {
            uint64_t key = it->from ? it->from : it->addr;
            auto& child = cur->children[key];
            if (!child) {
                child = std::make_unique<Node>();
                child->addr     = it->addr;
                child->from     = it->from;
                child->sym      = it->symbol.empty()
                                    ? resolveLabel(it->from ? it->from : it->addr).toStdString()
                                    : it->symbol;
                child->firstSeq = s.firstSeq;
            }
            child->hits += s.hits;
            cur = child.get();
        }
    }

    // 递归填充，hits 降序
    std::function<void(QTreeWidgetItem*, Node*)> fill = [&](QTreeWidgetItem* parent, Node* n){
        std::vector<Node*> kids;
        kids.reserve(n->children.size());
        for (auto& kv : n->children) kids.push_back(kv.second.get());
        std::sort(kids.begin(), kids.end(), [](Node* a, Node* b){
            if (a->hits != b->hits) return a->hits > b->hits;
            return a->firstSeq < b->firstSeq;
        });
        for (auto* k : kids) {
            QTreeWidgetItem* it = parent
                ? new QTreeWidgetItem(parent)
                : new QTreeWidgetItem(stackTree_);
            QString sym = QString::fromStdString(k->sym);
            uint64_t showAddr = k->from ? k->from : k->addr;
            if (sym.isEmpty()) sym = fmtVa(showAddr);
            it->setText(0, sym);
            it->setText(1, fmtVa(showAddr));
            it->setText(2, QString::number(k->hits));
            it->setText(3, QString::number(k->firstSeq));
            it->setData(0, kRoleAddr, QVariant::fromValue<qulonglong>(showAddr));
            it->setData(0, kRoleSym,  sym);
            it->setData(0, kRoleHits, k->hits);
            fill(it, k);
            if (!parent) it->setExpanded(true);
        }
    };
    fill(nullptr, &root);

    // K-05: 默认展开前两层，省去用户一个个点开
    stackTree_->expandToDepth(2);
}

QString TraceDialog::buildStackPathText(QTreeWidgetItem* leaf) const
{
    if (!leaf) return {};
    QStringList parts;
    QTreeWidgetItem* p = leaf;
    while (p) {
        parts.prepend(QStringLiteral("%1  @ %2  (hits=%3)")
                          .arg(p->text(0), p->text(1), p->text(2)));
        p = p->parent();
    }
    QString out;
    out += QStringLiteral("# 反向调用栈（线程入口 → 被断函数）\n\n");
    for (int i = 0; i < parts.size(); ++i) {
        out += QString(i * 2, QLatin1Char(' '));
        out += QStringLiteral("→ ");
        out += parts[i];
        out += QLatin1Char('\n');
    }
    return out;
}

void TraceDialog::populateViewRecursive(QTreeWidgetItem* parentItem, CallNodeView* node){
    if (!node) return;
    QTreeWidgetItem* it = parentItem
        ? new QTreeWidgetItem(parentItem)
        : new QTreeWidgetItem(tree_);

    QString sym = QString::fromStdString(node->sym);
    if (sym.isEmpty()) sym = fmtVa(node->addr);
    it->setText(0, sym);
    it->setText(1, node->addr ? fmtVa(node->addr) : QString());
    it->setText(2, QString::number(node->hits));
    it->setText(3, QString::number(node->firstSeq));
    it->setData(0, kRoleAddr, QVariant::fromValue<qulonglong>(node->addr));
    it->setData(0, kRoleSym,  sym);
    it->setData(0, kRoleHits, node->hits);

    // 折叠占位节点用稍暗的灰色，便于区分
    if (node->isFolded) {
        it->setForeground(0, QBrush(QColor(0x88, 0x88, 0x88)));
    }

    for (auto& c : node->children) {
        populateViewRecursive(it, c.get());
    }
    if (!parentItem) it->setExpanded(true);
}

void TraceDialog::updateStatus()
{
    auto& rec  = TraceRecorder::instance();
    auto& cst  = CallStackTracer::instance();

    if (isCallStackMode()) {
        QString state = cst.isArmed()
            ? QStringLiteral("◌ 等待命中")
            : QStringLiteral("○ 已停止");
        statusLbl_->setText(
            QStringLiteral("%1  [CallStack]  hits=%2/%3  unique=%4")
                .arg(state)
                .arg(cst.sampleCount())
                .arg(cst.maxSamples())
                .arg(cst.uniqueCount()));
        return;
    }

    std::size_t n = rec.eventCount();
    std::size_t nodes = 0, depth = 0;
    if (root_) CallGraph::summarize(root_.get(), &nodes, &depth);

    // 统计折叠后的视图节点数（递归 view_）
    std::size_t viewNodes = 0;
    if (view_) {
        std::function<void(CallNodeView*)> walk = [&](CallNodeView* v){
            for (auto& c : v->children) { ++viewNodes; walk(c.get()); }
        };
        walk(view_.get());
    }

    QString state;
    if (rec.hasArmed())          state = QStringLiteral("◌ 等待命中");
    else if (rec.isRecording())  state = QStringLiteral("● 录制中");
    else                         state = QStringLiteral("○ 已停止");

    QString modeStr;
    switch (rec.mode()) {
    case TraceMode::Targeted: modeStr = QStringLiteral("Targeted"); break;
    case TraceMode::Global:   modeStr = QStringLiteral("Global");   break;
    }

    statusLbl_->setText(
        QStringLiteral("%1  [%2]  events=%3  nodes=%4 (view=%5)  depth=%6")
            .arg(state).arg(modeStr).arg(n).arg(nodes).arg(viewNodes).arg(depth));
}

void TraceDialog::updateButtons()
{
    auto& rec = TraceRecorder::instance();
    auto& cst = CallStackTracer::instance();
    bool busy = rec.isRecording() || rec.hasArmed() || cst.isArmed();
    btnStart_->setEnabled(!busy);
    btnStop_->setEnabled(busy);
    modeBox_->setEnabled(!busy);
    bool wantsAddr = (modeBox_->currentIndex() == kModeTargeted
                      || modeBox_->currentIndex() == kModeCallStack);
    addrEdit_->setEnabled(!busy && wantsAddr);
    resolveBtn_->setEnabled(!busy && wantsAddr);
    maxSamplesBox_->setEnabled(!busy);
}

// ====== 节点交互 ======

void TraceDialog::onItemClicked(QTreeWidgetItem* item, int /*col*/)
{
    if (!item) {
        aiBtn_->setEnabled(false);
        return;
    }
    QString path = buildPathText(item);
    detail_->setPlainText(path);
    aiBtn_->setEnabled(true);
}

QString TraceDialog::buildPathText(QTreeWidgetItem* leaf) const
{
    if (!leaf) return {};
    QStringList parts;
    QTreeWidgetItem* p = leaf;
    while (p) {
        QString sym  = p->text(0);
        QString addr = p->text(1);
        parts.prepend(QStringLiteral("%1  @ %2").arg(sym, addr));
        p = p->parent();
    }
    QString out;
    out += QStringLiteral("# 调用路径（root → leaf）\n\n");
    for (int i = 0; i < parts.size(); ++i) {
        out += QString(i * 2, QLatin1Char(' '));
        out += QStringLiteral("→ ");
        out += parts[i];
        out += QLatin1Char('\n');
    }
    out += QStringLiteral("\n# 详情\n");
    out += QStringLiteral("地址: %1\n").arg(leaf->text(1));
    out += QStringLiteral("符号: %1\n").arg(leaf->text(0));
    out += QStringLiteral("hits: %1\n").arg(leaf->text(2));
    out += QStringLiteral("first seq: %1\n").arg(leaf->text(3));
    return out;
}

void TraceDialog::onItemDoubleClicked(QTreeWidgetItem* item, int /*col*/)
{
    if (!item) return;
    qulonglong va = item->data(0, kRoleAddr).toULongLong();
    if (va == 0) return;
    char cmd[64];
    std::snprintf(cmd, sizeof(cmd), "disasm 0x%llX",
                  static_cast<unsigned long long>(va));
    DbgCmdExec(cmd);
}

void TraceDialog::onContextMenu(const QPoint& pos)
{
    auto* item = tree_->itemAt(pos);
    if (!item) return;
    qulonglong va = item->data(0, kRoleAddr).toULongLong();

    QMenu menu(this);
    auto* actGoto    = menu.addAction(QStringLiteral("跳转到反汇编"));
    auto* actCopyVa  = menu.addAction(QStringLiteral("复制地址"));
    auto* actCopySym = menu.addAction(QStringLiteral("复制符号"));
    menu.addSeparator();
    auto* actFindCallers = menu.addAction(QStringLiteral("反向查找调用者"));
    auto* actAi          = menu.addAction(QStringLiteral("让 AI 分析此处"));

    auto* chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == actGoto && va) {
        char cmd[64];
        std::snprintf(cmd, sizeof(cmd), "disasm 0x%llX",
                      static_cast<unsigned long long>(va));
        DbgCmdExec(cmd);
    } else if (chosen == actCopyVa) {
        QApplication::clipboard()->setText(fmtVa(va));
    } else if (chosen == actCopySym) {
        QApplication::clipboard()->setText(item->text(0));
    } else if (chosen == actFindCallers && root_ && va) {
        auto paths = CallGraph::findCallers(root_.get(), va);
        QString out = QStringLiteral("# 反向调用者：找到 %1 条路径\n\n").arg(paths.size());
        int idx = 1;
        for (const auto& p : paths) {
            out += QStringLiteral("## 路径 %1\n").arg(idx++);
            for (std::size_t i = 0; i < p.nodes.size(); ++i) {
                out += QString(static_cast<int>(i * 2), QLatin1Char(' '));
                out += QStringLiteral("→ %1  @ %2\n")
                          .arg(QString::fromStdString(p.nodes[i]->sym))
                          .arg(fmtVa(p.nodes[i]->addr));
            }
            out += QLatin1Char('\n');
        }
        detail_->setPlainText(out);
        aiBtn_->setEnabled(!paths.empty());
    } else if (chosen == actAi && va) {
        char cmd[64];
        std::snprintf(cmd, sizeof(cmd), "disasm 0x%llX",
                      static_cast<unsigned long long>(va));
        DbgCmdExec(cmd);
        AssistantPanel::analyzeCurrentAddress();
    }
}

void TraceDialog::onAiAnalyze()
{
    QString detail = detail_->toPlainText();
    if (detail.isEmpty()) return;

    std::string prompt;
    prompt.reserve(static_cast<std::size_t>(detail.size()) + 256);
    if (isCallStackMode()) {
        prompt.append("以下是某个 API（典型如 send/WSASend/NtWriteFile）被命中时的"
                      "反向调用栈采样，最外层是线程入口，最内层是该 API 自身。"
                      "请推断每一层调用者的角色（业务/封装/CRT/系统），"
                      "并指出最可能负责'组包/参数构造'的业务函数（中文回答，"
                      "先一句话总结，再分点列出）：\n\n");
    } else {
        prompt.append("请分析以下调用链路径，推断各函数的角色与整体意图，"
                      "并指出可疑/敏感行为（中文回答，先给一句话总结再分点列出）：\n\n");
    }
    prompt.append(detail.toUtf8().constData());

    AssistantPanel::submitExternalPrompt(
        QStringLiteral("AI 分析调用链（节点 / 路径）"),
        prompt);
}

}  // namespace x64ai
