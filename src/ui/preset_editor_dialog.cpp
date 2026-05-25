// ui/preset_editor_dialog.cpp
#include "ui/preset_editor_dialog.h"

#include "ai/preset_store.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_policy.h"
#include "ai/tools/tool_registry.h"
#include "ai/chat_provider.h"
#include "util/logging.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QSplitter>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace x64ai {

namespace {

QString fromStd(const std::string& s) { return QString::fromStdString(s); }
std::string toStd(const QString& q)   { return q.toStdString(); }

std::string newPresetId()
{
    return QUuid::createUuid()
        .toString(QUuid::WithoutBraces)
        .left(8)
        .toStdString();
}

}  // namespace

QString PresetEditorDialog::prettyGroupName(const std::string& g)
{
    static const std::unordered_map<std::string, QString> map = {
        // 工具组
        {"static-info",         QStringLiteral("静态信息")},
        {"disasm-cfg",          QStringLiteral("反汇编 / CFG")},
        {"memory-search",       QStringLiteral("内存 / 搜索")},
        {"register-stack",      QStringLiteral("寄存器 / 栈")},
        {"breakpoint",          QStringLiteral("断点")},
        {"execution-control",   QStringLiteral("执行控制")},
        {"annotation",          QStringLiteral("标签 / 注释")},
        {"write-patch",         QStringLiteral("写 / 补丁")},
        {"gui-misc",            QStringLiteral("GUI / 杂项")},
        {"agent-meta",          QStringLiteral("脚本 / 元工具")},
        {"anti-debug-insight",  QStringLiteral("反调试洞察")},
        {"forensics",           QStringLiteral("取证")},
        // 预设组
        {"general",             QStringLiteral("通用")},
        {"exploration",         QStringLiteral("探索")},
        {"cracking",            QStringLiteral("破解 / 补丁")},
        {"tracing",             QStringLiteral("追踪")},
        {"scenarios",           QStringLiteral("场景")},
    };
    auto it = map.find(g);
    if (it != map.end()) return it->second + QStringLiteral(" (") + fromStd(g) + QStringLiteral(")");
    return fromStd(g);
}

QString PresetEditorDialog::prettyCategoryName(int cat)
{
    switch (static_cast<ToolCategory>(cat)) {
        case ToolCategory::Read:       return QStringLiteral("Read");
        case ToolCategory::DbgControl: return QStringLiteral("DbgCtrl");
        case ToolCategory::Write:      return QStringLiteral("Write");
    }
    return QStringLiteral("?");
}

PresetEditorDialog::PresetEditorDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Agent 预设管理"));
    resize(1000, 660);
    buildUi();

    // 确保预设已加载（万一调用方没 load 过）
    PresetStore::instance().load();
    reloadList();
}

void PresetEditorDialog::buildUi()
{
    // ---------- 左侧：QTreeWidget(group → preset) ----------
    listWidget_ = new QTreeWidget(this);
    listWidget_->setHeaderHidden(true);
    listWidget_->setMinimumWidth(240);
    listWidget_->setRootIsDecorated(true);
    listWidget_->setExpandsOnDoubleClick(false);

    newBtn_    = new QPushButton(QStringLiteral("新建"), this);
    cloneBtn_  = new QPushButton(QStringLiteral("复制"), this);
    deleteBtn_ = new QPushButton(QStringLiteral("删除"), this);
    resetBtn_  = new QPushButton(QStringLiteral("恢复出厂"), this);
    resetBtn_->setToolTip(QStringLiteral(
        "把所有出厂（\xF0\x9F\x94\x92）预设重置为内置版本；用户自定义预设不受影响。"));

    auto* leftBtnRow1 = new QHBoxLayout();
    leftBtnRow1->setSpacing(4);
    leftBtnRow1->addWidget(newBtn_);
    leftBtnRow1->addWidget(cloneBtn_);
    leftBtnRow1->addWidget(deleteBtn_);

    auto* leftLayout = new QVBoxLayout();
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);
    leftLayout->addWidget(listWidget_, 1);
    leftLayout->addLayout(leftBtnRow1);
    leftLayout->addWidget(resetBtn_);

    auto* leftBox = new QWidget(this);
    leftBox->setLayout(leftLayout);

    // ---------- 右侧表单 ----------
    badgeLabel_ = new QLabel(this);
    badgeLabel_->setStyleSheet(QStringLiteral(
        "QLabel { background:#2D2F33; color:#B7C0CC; padding:6px 10px; "
        "border-radius:4px; font-size:11px; }"));
    badgeLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    idEdit_ = new QLineEdit(this);
    idEdit_->setReadOnly(true);
    idEdit_->setStyleSheet(QStringLiteral("QLineEdit { color:#888; }"));

    nameEdit_ = new QLineEdit(this);
    descEdit_ = new QLineEdit(this);

    providerBox_ = new QComboBox(this);
    providerBox_->addItem(QStringLiteral("(沿用当前激活)"), QString());
    providerBox_->addItem(QStringLiteral("DeepSeek"),         QStringLiteral("deepseek"));
    providerBox_->addItem(QStringLiteral("GitHub Copilot"),   QStringLiteral("copilot"));

    modelEdit_ = new QLineEdit(this);
    modelEdit_->setPlaceholderText(QStringLiteral("留空 = provider 默认模型"));

    maxIterSpin_ = new QSpinBox(this);
    maxIterSpin_->setRange(1, 50);
    maxIterSpin_->setValue(20);

    tempSpin_ = new QDoubleSpinBox(this);
    tempSpin_->setRange(0.0, 1.5);
    tempSpin_->setSingleStep(0.1);
    tempSpin_->setDecimals(2);
    tempSpin_->setValue(0.2);

    showCtxCheck_ = new QCheckBox(QStringLiteral("在反汇编右键 AI 子菜单中显示"), this);

    // S9：预设 group / tags
    groupBox_ = new QComboBox(this);
    for (const char* g : { "general", "exploration", "cracking", "tracing", "scenarios" }) {
        groupBox_->addItem(prettyGroupName(g), QString::fromLatin1(g));
    }
    tagsEdit_ = new QLineEdit(this);
    tagsEdit_->setPlaceholderText(QStringLiteral(
        "逗号分隔，如：read-only, write, hw-bp, cfg, patch, annotation, dataflow, anti-debug"));

    sysPromptEdit_ = new QPlainTextEdit(this);
    sysPromptEdit_->setMinimumHeight(120);
    sysPromptEdit_->setPlaceholderText(
        QStringLiteral("role=system 初始消息。空 = 用全局默认。建议结尾追加：请用简体中文回答。"));

    userTplEdit_ = new QPlainTextEdit(this);
    userTplEdit_->setMinimumHeight(120);
    userTplEdit_->setPlaceholderText(
        QStringLiteral("用户消息模板。可用占位符：{{cip}} {{module}} {{selection}} {{disasm}} {{user}}"));

    // ---------- 工具白名单（S9：分组 + 搜索 + chip） ----------
    toolSearchEdit_ = new QLineEdit(this);
    toolSearchEdit_->setPlaceholderText(QStringLiteral("搜索工具名 / 描述..."));
    toolSearchEdit_->setClearButtonEnabled(true);

    auto makeChip = [this](const QString& text, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setChecked(true);
        b->setAutoRaise(false);
        b->setStyleSheet(QStringLiteral(
            "QToolButton { padding:2px 8px; border:1px solid #444; border-radius:10px; "
            "background:#2A2C30; color:#B7C0CC; }"
            "QToolButton:checked { background:#3D5AFE; color:white; border-color:#3D5AFE; }"));
        return b;
    };
    chipReadBtn_  = makeChip(QStringLiteral("Read"),
                             QStringLiteral("显示只读工具"));
    chipCtrlBtn_  = makeChip(QStringLiteral("DbgCtrl"),
                             QStringLiteral("显示调试控制工具"));
    chipWriteBtn_ = makeChip(QStringLiteral("Write"),
                             QStringLiteral("显示写工具"));

    toolsTree_ = new QTreeWidget(this);
    toolsTree_->setHeaderLabels({ QStringLiteral("工具 / 分组"),
                                  QStringLiteral("类别"),
                                  QStringLiteral("描述") });
    toolsTree_->setMinimumHeight(200);
    toolsTree_->setColumnWidth(0, 220);
    toolsTree_->setColumnWidth(1, 70);
    toolsTree_->setRootIsDecorated(true);
    toolsTree_->setAlternatingRowColors(true);
    // S9++：表头可见 + 可点击排序 + 描述列换行
    toolsTree_->setHeaderHidden(false);
    toolsTree_->setSortingEnabled(true);
    toolsTree_->header()->setSortIndicatorShown(true);
    toolsTree_->header()->setSectionsClickable(true);
    toolsTree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    toolsTree_->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    toolsTree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    toolsTree_->setWordWrap(true);
    toolsTree_->setTextElideMode(Qt::ElideNone);
    toolsTree_->setUniformRowHeights(false);
    toolsTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    // 默认按"工具 / 分组"列升序（组节点优先级在 buildToolsTree 用排序键控制）
    toolsTree_->sortItems(0, Qt::AscendingOrder);

    toolsAllBtn_     = new QPushButton(QStringLiteral("全选"), this);
    toolsNoneBtn_    = new QPushButton(QStringLiteral("全清"), this);
    toolsReadOnlyBtn_= new QPushButton(QStringLiteral("仅勾 Read"), this);
    toolsReadOnlyBtn_->setToolTip(QStringLiteral(
        "只勾选 Read 类工具（去掉所有 Write / DbgControl）"));
    // S9++：「已勾选 N/M」实时计数
    toolsCountLabel_ = new QLabel(QStringLiteral("已勾选 0 / 0"), this);
    toolsCountLabel_->setStyleSheet(QStringLiteral(
        "color:#9DA5B0; padding:0 8px;"));
    toolsCountLabel_->setToolTip(QStringLiteral(
        "「已勾选 / 当前过滤可见」工具数；空集等价于「全选」"));

    auto* searchRow = new QHBoxLayout();
    searchRow->setSpacing(6);
    searchRow->addWidget(toolSearchEdit_, 1);
    searchRow->addWidget(chipReadBtn_);
    searchRow->addWidget(chipCtrlBtn_);
    searchRow->addWidget(chipWriteBtn_);
    searchRow->addWidget(toolsCountLabel_);

    auto* toolsBtnRow = new QHBoxLayout();
    toolsBtnRow->setSpacing(4);
    toolsBtnRow->addStretch(1);
    toolsBtnRow->addWidget(toolsReadOnlyBtn_);
    toolsBtnRow->addWidget(toolsAllBtn_);
    toolsBtnRow->addWidget(toolsNoneBtn_);

    auto* toolsBox = new QGroupBox(QStringLiteral("可调用工具"), this);
    {
        auto* lay = new QVBoxLayout(toolsBox);
        lay->setContentsMargins(8, 4, 8, 8);
        lay->setSpacing(4);
        lay->addLayout(searchRow);
        lay->addWidget(toolsTree_, 1);
        lay->addLayout(toolsBtnRow);
    }

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(6);
    form->addRow(QStringLiteral("ID"),         idEdit_);
    form->addRow(QStringLiteral("名称"),       nameEdit_);
    form->addRow(QStringLiteral("描述"),       descEdit_);
    form->addRow(QStringLiteral("分组"),       groupBox_);
    form->addRow(QStringLiteral("标签"),       tagsEdit_);
    form->addRow(QStringLiteral("Provider"),   providerBox_);
    form->addRow(QStringLiteral("Model"),      modelEdit_);
    {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);
        row->addWidget(new QLabel(QStringLiteral("maxIter:"), this));
        row->addWidget(maxIterSpin_);
        row->addSpacing(12);
        row->addWidget(new QLabel(QStringLiteral("temperature:"), this));
        row->addWidget(tempSpin_);
        row->addStretch(1);
        form->addRow(QStringLiteral("运行参数"), row);
    }
    form->addRow(QStringLiteral(""),                showCtxCheck_);
    form->addRow(QStringLiteral("System Prompt"),   sysPromptEdit_);
    form->addRow(QStringLiteral("User Template"),   userTplEdit_);
    form->addRow(toolsBox);

    saveBtn_  = new QPushButton(QStringLiteral("保存当前预设"), this);
    saveBtn_->setDefault(true);
    closeBtn_ = new QPushButton(QStringLiteral("关闭"), this);

    auto* rightBtnRow = new QHBoxLayout();
    rightBtnRow->addStretch(1);
    rightBtnRow->addWidget(saveBtn_);
    rightBtnRow->addWidget(closeBtn_);

    auto* rightLayout = new QVBoxLayout();
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(6);

    // S9+：badge 行 = badgeLabel_ + 右侧 lockBtn_
    lockBtn_ = new QPushButton(this);
    lockBtn_->setCursor(Qt::PointingHandCursor);
    lockBtn_->setFocusPolicy(Qt::NoFocus);
    lockBtn_->hide();  // 由 populateForm 决定可见性与文案
    auto* badgeRow = new QHBoxLayout();
    badgeRow->setContentsMargins(0, 0, 0, 0);
    badgeRow->addWidget(badgeLabel_, 1);
    badgeRow->addWidget(lockBtn_, 0);
    rightLayout->addLayout(badgeRow);
    rightLayout->addLayout(form, 1);
    rightLayout->addLayout(rightBtnRow);

    auto* rightBox = new QWidget(this);
    rightBox->setLayout(rightLayout);

    // ---------- 总布局 ----------
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(leftBox);
    splitter->addWidget(rightBox);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->addWidget(splitter, 1);

    // ---------- 信号 ----------
    connect(listWidget_, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem*, QTreeWidgetItem*) { onListCurrentChanged(); });
    connect(newBtn_,    &QPushButton::clicked, this, &PresetEditorDialog::onNewClicked);
    connect(cloneBtn_,  &QPushButton::clicked, this, &PresetEditorDialog::onCloneClicked);
    connect(deleteBtn_, &QPushButton::clicked, this, &PresetEditorDialog::onDeleteClicked);
    connect(resetBtn_,  &QPushButton::clicked, this, &PresetEditorDialog::onResetDefaultsClicked);
    connect(saveBtn_,   &QPushButton::clicked, this, &PresetEditorDialog::onSaveCurrentClicked);
    connect(closeBtn_,  &QPushButton::clicked, this, &PresetEditorDialog::onCloseClicked);
    connect(lockBtn_,   &QPushButton::clicked, this, &PresetEditorDialog::onToggleLockClicked);

    // 所有可编辑字段：变更即 markDirty
    auto wireDirty = [this](QWidget* w) {
        if (auto* le = qobject_cast<QLineEdit*>(w))
            connect(le, &QLineEdit::textChanged, this, [this](const QString&) { markDirty(); });
        else if (auto* te = qobject_cast<QPlainTextEdit*>(w))
            connect(te, &QPlainTextEdit::textChanged, this, [this]() { markDirty(); });
        else if (auto* sp = qobject_cast<QSpinBox*>(w))
            connect(sp, QOverload<int>::of(&QSpinBox::valueChanged),
                    this, [this](int) { markDirty(); });
        else if (auto* ds = qobject_cast<QDoubleSpinBox*>(w))
            connect(ds, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                    this, [this](double) { markDirty(); });
        else if (auto* cb = qobject_cast<QCheckBox*>(w))
            connect(cb, &QCheckBox::toggled, this, [this](bool) { markDirty(); });
        else if (auto* combo = qobject_cast<QComboBox*>(w))
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this](int) { markDirty(); });
    };
    wireDirty(nameEdit_);
    wireDirty(descEdit_);
    wireDirty(providerBox_);
    wireDirty(modelEdit_);
    wireDirty(maxIterSpin_);
    wireDirty(tempSpin_);
    wireDirty(showCtxCheck_);
    wireDirty(sysPromptEdit_);
    wireDirty(userTplEdit_);
    wireDirty(groupBox_);
    wireDirty(tagsEdit_);
    connect(toolsTree_, &QTreeWidget::itemChanged, this,
            &PresetEditorDialog::onToolGroupItemChanged);

    connect(toolSearchEdit_, &QLineEdit::textChanged,
            this, &PresetEditorDialog::onToolSearchChanged);
    connect(chipReadBtn_,  &QToolButton::toggled,
            this, [this](bool) { onToolFilterChipToggled(); });
    connect(chipCtrlBtn_,  &QToolButton::toggled,
            this, [this](bool) { onToolFilterChipToggled(); });
    connect(chipWriteBtn_, &QToolButton::toggled,
            this, [this](bool) { onToolFilterChipToggled(); });

    connect(toolsAllBtn_, &QPushButton::clicked, this, [this]() {
        for (int i = 0; i < toolsTree_->topLevelItemCount(); ++i) {
            auto* grp = toolsTree_->topLevelItem(i);
            for (int j = 0; j < grp->childCount(); ++j) {
                grp->child(j)->setCheckState(0, Qt::Checked);
            }
        }
    });
    connect(toolsNoneBtn_, &QPushButton::clicked, this, [this]() {
        for (int i = 0; i < toolsTree_->topLevelItemCount(); ++i) {
            auto* grp = toolsTree_->topLevelItem(i);
            for (int j = 0; j < grp->childCount(); ++j) {
                grp->child(j)->setCheckState(0, Qt::Unchecked);
            }
        }
    });
    connect(toolsReadOnlyBtn_, &QPushButton::clicked, this, [this]() {
        // 只勾 Read 工具
        for (int i = 0; i < toolsTree_->topLevelItemCount(); ++i) {
            auto* grp = toolsTree_->topLevelItem(i);
            for (int j = 0; j < grp->childCount(); ++j) {
                auto* leaf = grp->child(j);
                const int catInt = leaf->data(0, Qt::UserRole + 1).toInt();
                const bool isRead = static_cast<ToolCategory>(catInt) == ToolCategory::Read;
                leaf->setCheckState(0, isRead ? Qt::Checked : Qt::Unchecked);
            }
        }
    });

    // S9++：右键菜单 + 双击详情 + itemChanged 同步刷新计数
    connect(toolsTree_, &QWidget::customContextMenuRequested,
            this, &PresetEditorDialog::onToolsContextMenu);
    connect(toolsTree_, &QTreeWidget::itemDoubleClicked,
            this, &PresetEditorDialog::onToolItemDoubleClicked);
    connect(toolsTree_, &QTreeWidget::itemChanged,
            this, [this](QTreeWidgetItem*, int) { updateToolsCount(); });
}

QString PresetEditorDialog::makeListItemLabel(const AgentPreset& p)
{
    QString lock = p.readonly ? QStringLiteral("\xF0\x9F\x94\x92 ") : QString();
    return lock + fromStd(p.name);
}

void PresetEditorDialog::reloadList(const std::string& selectId)
{
    listWidget_->blockSignals(true);
    listWidget_->clear();

    const auto& all = PresetStore::instance().presets();

    // 按 group 分组聚合（保持稳定顺序：先 general / exploration / cracking / tracing /
    // scenarios，其余按出现顺序追加）
    static const std::vector<std::string> kOrder = {
        "general", "exploration", "cracking", "tracing", "scenarios"
    };
    std::map<std::string, std::vector<const AgentPreset*>> byGroup;
    std::vector<std::string> groupOrder;
    auto pushGroup = [&](const std::string& g) {
        if (byGroup.find(g) == byGroup.end()) {
            byGroup[g];
            groupOrder.push_back(g);
        }
    };
    for (const auto& g : kOrder) pushGroup(g);
    for (const auto& p : all) {
        std::string g = p.group.empty() ? "general" : p.group;
        pushGroup(g);
        byGroup[g].push_back(&p);
    }

    QTreeWidgetItem* selectItem = nullptr;
    for (const auto& g : groupOrder) {
        const auto& list = byGroup[g];
        if (list.empty()) continue;
        auto* groupNode = new QTreeWidgetItem(listWidget_);
        groupNode->setText(0, QStringLiteral("%1   [%2]")
                                  .arg(prettyGroupName(g))
                                  .arg(list.size()));
        groupNode->setFirstColumnSpanned(true);
        QFont f = groupNode->font(0);
        f.setBold(true);
        groupNode->setFont(0, f);
        groupNode->setExpanded(true);
        groupNode->setFlags(groupNode->flags() & ~Qt::ItemIsSelectable);

        for (const auto* p : list) {
            auto* leaf = new QTreeWidgetItem(groupNode);
            leaf->setText(0, makeListItemLabel(*p));
            leaf->setData(0, Qt::UserRole, fromStd(p->id));
            if (!p->description.empty())
                leaf->setToolTip(0, fromStd(p->description));
            if (!selectId.empty() && p->id == selectId) selectItem = leaf;
        }
    }
    listWidget_->blockSignals(false);

    if (listWidget_->topLevelItemCount() == 0) {
        currentId_.clear();
        currentReadonly_ = false;
        setFormEnabled(false);
        return;
    }

    if (!selectItem) {
        // 选第一个叶子
        for (int i = 0; i < listWidget_->topLevelItemCount() && !selectItem; ++i) {
            auto* grp = listWidget_->topLevelItem(i);
            if (grp->childCount() > 0) selectItem = grp->child(0);
        }
    }
    if (selectItem) listWidget_->setCurrentItem(selectItem);
}

void PresetEditorDialog::onListCurrentChanged()
{
    if (!maybeAskDiscardChanges()) {
        // 用户取消 → 还原选中项到 currentId_
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> findById =
            [&](QTreeWidgetItem* root) -> QTreeWidgetItem* {
                for (int i = 0; i < root->childCount(); ++i) {
                    auto* c = root->child(i);
                    if (c->data(0, Qt::UserRole).toString().toStdString() == currentId_) return c;
                    if (auto* deep = findById(c)) return deep;
                }
                return nullptr;
            };
        for (int i = 0; i < listWidget_->topLevelItemCount(); ++i) {
            if (auto* hit = findById(listWidget_->topLevelItem(i))) {
                listWidget_->blockSignals(true);
                listWidget_->setCurrentItem(hit);
                listWidget_->blockSignals(false);
                return;
            }
        }
        return;
    }
    auto* cur = listWidget_->currentItem();
    if (!cur || cur->childCount() > 0) { setFormEnabled(false); return; }  // 组节点不响应
    std::string id = cur->data(0, Qt::UserRole).toString().toStdString();
    if (id.empty()) { setFormEnabled(false); return; }
    auto p = PresetStore::instance().findById(id);
    if (!p) { setFormEnabled(false); return; }
    populateForm(*p);
}

void PresetEditorDialog::populateForm(const AgentPreset& p)
{
    suppressDirty_ = true;
    currentId_       = p.id;
    currentReadonly_ = p.readonly;

    idEdit_->setText(fromStd(p.id));
    nameEdit_->setText(fromStd(p.name));
    descEdit_->setText(fromStd(p.description));

    int provIdx = providerBox_->findData(fromStd(p.provider));
    providerBox_->setCurrentIndex(provIdx >= 0 ? provIdx : 0);

    modelEdit_->setText(fromStd(p.model));
    maxIterSpin_->setValue(p.maxIter);
    tempSpin_->setValue(p.temperature);
    showCtxCheck_->setChecked(p.showInContextMenu);
    sysPromptEdit_->setPlainText(fromStd(p.systemPrompt));
    userTplEdit_->setPlainText(fromStd(p.userTemplate));

    // S9：group / tags
    {
        const QString g = fromStd(p.group.empty() ? std::string("general") : p.group);
        int gi = groupBox_->findData(g);
        groupBox_->setCurrentIndex(gi >= 0 ? gi : 0);
    }
    {
        QStringList ts;
        for (const auto& t : p.tags) ts << fromStd(t);
        tagsEdit_->setText(ts.join(QStringLiteral(", ")));
    }

    loadEnabledTools(p.enabledTools);
    updateBadge(p);

    setFormEnabled(!p.readonly);
    deleteBtn_->setEnabled(true);  // 用户要求：readonly 也允许删
    saveBtn_->setEnabled(!p.readonly);

    // S9+：lockBtn_ 任何预设都显示，文案随状态切换
    lockBtn_->show();
    if (p.readonly) {
        lockBtn_->setText(QStringLiteral("\xF0\x9F\x94\x92 \xE5\xB7\xB2\xE9\x94\x81\xE5\xAE\x9A \xC2\xB7 \xE8\xA7\xA3\xE9\x94\x81\xE7\xBC\x96\xE8\xBE\x91"));  // 🔒 已锁定 · 解锁编辑
        lockBtn_->setToolTip(QStringLiteral("出厂预设默认只读。解锁后可修改并覆盖保存。"));
    } else {
        lockBtn_->setText(QStringLiteral("\xF0\x9F\x94\x93 \xE5\xB7\xB2\xE8\xA7\xA3\xE9\x94\x81 \xC2\xB7 \xE9\x87\x8D\xE6\x96\xB0\xE9\x94\x81\xE5\xAE\x9A"));  // 🔓 已解锁 · 重新锁定
        lockBtn_->setToolTip(QStringLiteral("重新锁定后将不可编辑（防误改）。"));
    }

    suppressDirty_ = false;
    dirty_ = false;
}

void PresetEditorDialog::buildToolsTree()
{
    // 一次性构建分组树骨架（不勾选状态）；后续 loadEnabledTools 只刷 check
    toolsTree_->blockSignals(true);
    // 构建期间关闭排序，避免叶子被字典序插乱；构建完按 kOrder 自然展示
    const bool prevSort = toolsTree_->isSortingEnabled();
    toolsTree_->setSortingEnabled(false);
    toolsTree_->clear();

    auto& reg = ToolRegistry::instance();
    auto chatTools = reg.listChatTools();

    // group → vector<ChatTool*>
    std::map<std::string, std::vector<const ChatTool*>> byGroup;
    for (const auto& t : chatTools) {
        const std::string g = reg.groupOf(t.name);
        byGroup[g].push_back(&t);
    }
    // 按 §5.4 顺序
    static const std::vector<std::string> kOrder = {
        "static-info", "disasm-cfg", "memory-search", "register-stack",
        "breakpoint", "execution-control", "annotation", "write-patch",
        "gui-misc", "agent-meta", "anti-debug-insight", "forensics",
    };
    std::vector<std::string> order;
    for (const auto& g : kOrder) if (byGroup.count(g)) order.push_back(g);
    for (const auto& [g, _] : byGroup)
        if (std::find(order.begin(), order.end(), g) == order.end()) order.push_back(g);

    // 类别 → 前景/背景配色（Dark Modern 调色）
    auto catColors = [](ToolCategory c) -> std::pair<QColor, QColor> {
        switch (c) {
            case ToolCategory::Read:       return { QColor(0x6F, 0xCF, 0x97), QColor(0x1B, 0x3A, 0x2A) };
            case ToolCategory::DbgControl: return { QColor(0xF2, 0xC8, 0x4B), QColor(0x3D, 0x33, 0x12) };
            case ToolCategory::Write:      return { QColor(0xEB, 0x5C, 0x5C), QColor(0x40, 0x1F, 0x1F) };
        }
        return { QColor(0xB7, 0xC0, 0xCC), QColor() };
    };

    for (const auto& g : order) {
        auto& list = byGroup[g];
        std::sort(list.begin(), list.end(),
                  [](const ChatTool* a, const ChatTool* b) { return a->name < b->name; });

        auto* grp = new QTreeWidgetItem(toolsTree_);
        grp->setText(0, QStringLiteral("%1   [%2]")
                            .arg(prettyGroupName(g)).arg(list.size()));
        grp->setData(0, Qt::UserRole, QStringLiteral("__group__"));
        grp->setData(0, Qt::UserRole + 2, fromStd(g));   // 原始 group key
        grp->setFirstColumnSpanned(false);
        QFont f = grp->font(0); f.setBold(true); grp->setFont(0, f);
        grp->setFlags(grp->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        grp->setCheckState(0, Qt::Unchecked);
        grp->setExpanded(true);

        for (const auto* t : list) {
            auto* leaf = new QTreeWidgetItem(grp);
            const ToolCategory cat = reg.categoryOf(t->name);
            const char* catTxt =
                cat == ToolCategory::Read       ? "Read"  :
                cat == ToolCategory::DbgControl ? "Ctrl"  :
                cat == ToolCategory::Write      ? "Write" : "?";
            const std::string& descShow = !t->descriptionZh.empty() ? t->descriptionZh : t->description;
            leaf->setText(0, fromStd(t->name));
            leaf->setText(1, QString::fromLatin1(catTxt));
            leaf->setText(2, fromStd(descShow));
            leaf->setToolTip(0, fromStd(descShow));
            leaf->setToolTip(1, QStringLiteral(
                "Read：只读，不改状态\n"
                "Ctrl：控制运行/断点等可逆操作\n"
                "Write：写内存/寄存器/补丁（高风险）"));
            leaf->setToolTip(2, fromStd(descShow));
            leaf->setData(0, Qt::UserRole, fromStd(t->name));
            leaf->setData(0, Qt::UserRole + 1, static_cast<int>(cat));
            leaf->setFlags(leaf->flags() | Qt::ItemIsUserCheckable);
            leaf->setCheckState(0, Qt::Unchecked);
            // 类别列颜色徽标 + 居中
            auto [fg, bg] = catColors(cat);
            leaf->setForeground(1, QBrush(fg));
            if (bg.isValid()) leaf->setBackground(1, QBrush(bg));
            leaf->setTextAlignment(1, Qt::AlignCenter);
            QFont lf = leaf->font(1); lf.setBold(true); leaf->setFont(1, lf);
        }
    }
    toolsTree_->setSortingEnabled(prevSort);
    toolsTree_->blockSignals(false);
}

void PresetEditorDialog::loadEnabledTools(const std::vector<std::string>& enabled)
{
    // 第一次需要构建树骨架
    if (toolsTree_->topLevelItemCount() == 0) {
        buildToolsTree();
    }

    std::set<std::string> enabledSet(enabled.begin(), enabled.end());
    const bool selectAll = enabled.empty();  // 约定：空集 = 全部

    toolsTree_->blockSignals(true);
    for (int i = 0; i < toolsTree_->topLevelItemCount(); ++i) {
        auto* grp = toolsTree_->topLevelItem(i);
        for (int j = 0; j < grp->childCount(); ++j) {
            auto* leaf = grp->child(j);
            const std::string n = leaf->data(0, Qt::UserRole).toString().toStdString();
            const bool on = selectAll || enabledSet.count(n) > 0;
            leaf->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
        }
    }
    toolsTree_->blockSignals(false);
    applyToolFilter();
    updateToolsCount();
}

std::vector<std::string> PresetEditorDialog::collectEnabledTools() const
{
    std::vector<std::string> out;
    int total = 0;
    int checked = 0;
    for (int i = 0; i < toolsTree_->topLevelItemCount(); ++i) {
        auto* grp = toolsTree_->topLevelItem(i);
        for (int j = 0; j < grp->childCount(); ++j) {
            ++total;
            if (grp->child(j)->checkState(0) == Qt::Checked) ++checked;
        }
    }
    // 全部勾选 = 等价于"留空"
    if (checked == total) return out;
    for (int i = 0; i < toolsTree_->topLevelItemCount(); ++i) {
        auto* grp = toolsTree_->topLevelItem(i);
        for (int j = 0; j < grp->childCount(); ++j) {
            auto* leaf = grp->child(j);
            if (leaf->checkState(0) == Qt::Checked) {
                out.push_back(leaf->data(0, Qt::UserRole).toString().toStdString());
            }
        }
    }
    return out;
}

void PresetEditorDialog::onToolSearchChanged(const QString&) { applyToolFilter(); }
void PresetEditorDialog::onToolFilterChipToggled()           { applyToolFilter(); }
void PresetEditorDialog::onToolGroupItemChanged(QTreeWidgetItem*, int) { markDirty(); }

void PresetEditorDialog::applyToolFilter()
{
    const QString needle = toolSearchEdit_->text().trimmed().toLower();
    const bool showRead  = chipReadBtn_->isChecked();
    const bool showCtrl  = chipCtrlBtn_->isChecked();
    const bool showWrite = chipWriteBtn_->isChecked();

    for (int i = 0; i < toolsTree_->topLevelItemCount(); ++i) {
        auto* grp = toolsTree_->topLevelItem(i);
        int visibleLeafs = 0;
        for (int j = 0; j < grp->childCount(); ++j) {
            auto* leaf = grp->child(j);
            const int catInt = leaf->data(0, Qt::UserRole + 1).toInt();
            const auto cat = static_cast<ToolCategory>(catInt);
            bool catOk = (cat == ToolCategory::Read       && showRead)
                      || (cat == ToolCategory::DbgControl && showCtrl)
                      || (cat == ToolCategory::Write      && showWrite);
            bool nameOk = needle.isEmpty()
                       || leaf->text(0).toLower().contains(needle)
                       || leaf->text(2).toLower().contains(needle);
            bool hide = !(catOk && nameOk);
            leaf->setHidden(hide);
            if (!hide) ++visibleLeafs;
        }
        grp->setHidden(visibleLeafs == 0);
    }
    updateToolsCount();
}

void PresetEditorDialog::updateBadge(const AgentPreset& p)
{
    QStringList parts;
    const int toolN = p.enabledTools.empty()
                          ? static_cast<int>(ToolRegistry::instance().listChatTools().size())
                          : static_cast<int>(p.enabledTools.size());
    parts << QStringLiteral("%1 工具").arg(toolN);
    parts << prettyGroupName(p.group.empty() ? std::string("general") : p.group);
    if (!p.tags.empty()) {
        QStringList ts;
        for (const auto& t : p.tags) ts << fromStd(t);
        parts << QStringLiteral("[%1]").arg(ts.join(QStringLiteral(", ")));
    }
    parts << QStringLiteral("provider: %1")
                  .arg(p.provider.empty()
                           ? QStringLiteral("(当前激活)")
                           : fromStd(p.provider));
    if (p.readonly) parts << QStringLiteral("\xF0\x9F\x94\x92 readonly");
    badgeLabel_->setText(parts.join(QStringLiteral("   \xC2\xB7   ")));
}

void PresetEditorDialog::updateToolsCount()
{
    int total = 0, checked = 0, visible = 0, visibleChecked = 0;
    for (int i = 0; i < toolsTree_->topLevelItemCount(); ++i) {
        auto* grp = toolsTree_->topLevelItem(i);
        for (int j = 0; j < grp->childCount(); ++j) {
            auto* leaf = grp->child(j);
            ++total;
            const bool on = (leaf->checkState(0) == Qt::Checked);
            if (on) ++checked;
            if (!leaf->isHidden()) {
                ++visible;
                if (on) ++visibleChecked;
            }
        }
    }
    QString txt;
    if (visible == total) {
        txt = QStringLiteral("已勾选 %1 / %2").arg(checked).arg(total);
    } else {
        txt = QStringLiteral("已勾选 %1 / %2  (过滤后 %3 / %4)")
                  .arg(checked).arg(total).arg(visibleChecked).arg(visible);
    }
    toolsCountLabel_->setText(txt);
    // 全选时（约定 enabledTools=空）给个视觉提示
    if (checked == total) {
        toolsCountLabel_->setStyleSheet(QStringLiteral(
            "color:#6FCF97; padding:0 8px;"));
    } else if (checked == 0) {
        toolsCountLabel_->setStyleSheet(QStringLiteral(
            "color:#EB5C5C; padding:0 8px;"));
    } else {
        toolsCountLabel_->setStyleSheet(QStringLiteral(
            "color:#9DA5B0; padding:0 8px;"));
    }
}

void PresetEditorDialog::onToolsContextMenu(const QPoint& pos)
{
    auto* item = toolsTree_->itemAt(pos);
    if (!item) return;
    const bool isGroup = (item->data(0, Qt::UserRole).toString() == QStringLiteral("__group__"));

    QMenu menu(this);
    if (isGroup) {
        QString gname = item->data(0, Qt::UserRole + 2).toString();
        QAction* aAll  = menu.addAction(QStringLiteral("全选本组（%1）").arg(gname));
        QAction* aNone = menu.addAction(QStringLiteral("取消本组"));
        QAction* aInv  = menu.addAction(QStringLiteral("反选本组"));
        menu.addSeparator();
        QAction* aOnlyRead = menu.addAction(QStringLiteral("本组仅勾 Read"));
        QAction* picked = menu.exec(toolsTree_->viewport()->mapToGlobal(pos));
        if (!picked) return;

        auto walkLeaves = [&](auto&& fn) {
            for (int j = 0; j < item->childCount(); ++j) {
                auto* leaf = item->child(j);
                if (leaf->isHidden()) continue;  // 只动当前过滤可见的
                fn(leaf);
            }
        };
        if (picked == aAll) {
            walkLeaves([](QTreeWidgetItem* l){ l->setCheckState(0, Qt::Checked); });
        } else if (picked == aNone) {
            walkLeaves([](QTreeWidgetItem* l){ l->setCheckState(0, Qt::Unchecked); });
        } else if (picked == aInv) {
            walkLeaves([](QTreeWidgetItem* l){
                l->setCheckState(0,
                    l->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
            });
        } else if (picked == aOnlyRead) {
            walkLeaves([](QTreeWidgetItem* l){
                const auto c = static_cast<ToolCategory>(l->data(0, Qt::UserRole + 1).toInt());
                l->setCheckState(0, c == ToolCategory::Read ? Qt::Checked : Qt::Unchecked);
            });
        }
        markDirty();
    } else {
        // 叶子
        QAction* aCopy    = menu.addAction(QStringLiteral("复制工具名"));
        QAction* aDetails = menu.addAction(QStringLiteral("查看完整 schema / JSON..."));
        QAction* picked = menu.exec(toolsTree_->viewport()->mapToGlobal(pos));
        if (!picked) return;
        if (picked == aCopy) {
            QGuiApplication::clipboard()->setText(item->text(0));
        } else if (picked == aDetails) {
            showToolDetails(item);
        }
    }
}

void PresetEditorDialog::onToolItemDoubleClicked(QTreeWidgetItem* item, int column)
{
    if (!item) return;
    // 组节点双击：折叠/展开（Qt 默认行为已有，跳过）
    if (item->data(0, Qt::UserRole).toString() == QStringLiteral("__group__")) return;
    // 第 0 列双击会与"勾选/标签编辑"冲突 → 仅第 1/2 列触发详情；第 0 列也直接弹（Qt 默认禁止编辑）
    (void)column;
    showToolDetails(item);
}

void PresetEditorDialog::showToolDetails(QTreeWidgetItem* leaf)
{
    if (!leaf) return;
    const std::string name = leaf->data(0, Qt::UserRole).toString().toStdString();
    if (name.empty()) return;

    auto& reg = ToolRegistry::instance();
    if (!reg.has(name)) return;
    auto allTools = reg.listChatTools();
    const ChatTool* meta = nullptr;
    for (const auto& t : allTools) {
        if (t.name == name) { meta = &t; break; }
    }
    if (!meta) return;

    const ToolCategory cat = reg.categoryOf(name);
    const QString catTxt =
        cat == ToolCategory::Read       ? QStringLiteral("Read（只读）")  :
        cat == ToolCategory::DbgControl ? QStringLiteral("Ctrl（控制）") :
        cat == ToolCategory::Write      ? QStringLiteral("Write（高风险）") :
                                          QStringLiteral("?");
    const QString group = prettyGroupName(reg.groupOf(name));

    // pretty-print parametersJson
    QString prettySchema = fromStd(meta->parametersJson);
    try {
        auto j = nlohmann::json::parse(meta->parametersJson);
        prettySchema = fromStd(j.dump(2));
    } catch (...) {}

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("工具详情 - %1").arg(fromStd(name)));
    dlg.resize(680, 560);

    auto* header = new QLabel(&dlg);
    header->setTextFormat(Qt::RichText);
    header->setWordWrap(true);
    header->setText(QStringLiteral(
        "<table cellspacing='6'>"
        "<tr><td><b>名称</b></td><td><code>%1</code></td></tr>"
        "<tr><td><b>分组</b></td><td>%2</td></tr>"
        "<tr><td><b>类别</b></td><td>%3</td></tr>"
        "<tr><td valign='top'><b>描述</b></td><td>%4</td></tr>"
        "</table>")
        .arg(fromStd(name).toHtmlEscaped())
        .arg(group.toHtmlEscaped())
        .arg(catTxt.toHtmlEscaped())
        .arg(fromStd(!meta->descriptionZh.empty() ? meta->descriptionZh : meta->description).toHtmlEscaped()));

    auto* schemaTitle = new QLabel(QStringLiteral("<b>参数 schema (JSON)</b>"), &dlg);
    schemaTitle->setTextFormat(Qt::RichText);

    auto* schemaEdit = new QPlainTextEdit(&dlg);
    schemaEdit->setPlainText(prettySchema);
    schemaEdit->setReadOnly(true);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    schemaEdit->setFont(mono);
    schemaEdit->setLineWrapMode(QPlainTextEdit::NoWrap);

    auto* copyNameBtn   = new QPushButton(QStringLiteral("复制工具名"), &dlg);
    auto* copySchemaBtn = new QPushButton(QStringLiteral("复制 schema"), &dlg);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    bb->addButton(copyNameBtn,   QDialogButtonBox::ActionRole);
    bb->addButton(copySchemaBtn, QDialogButtonBox::ActionRole);
    connect(copyNameBtn, &QPushButton::clicked, &dlg, [name]() {
        QGuiApplication::clipboard()->setText(fromStd(name));
    });
    connect(copySchemaBtn, &QPushButton::clicked, &dlg, [schemaEdit]() {
        QGuiApplication::clipboard()->setText(schemaEdit->toPlainText());
    });
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);

    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(header);
    lay->addSpacing(4);
    lay->addWidget(schemaTitle);
    lay->addWidget(schemaEdit, 1);
    lay->addWidget(bb);

    dlg.exec();
}

AgentPreset PresetEditorDialog::gatherForm() const
{
    AgentPreset p;
    p.id                = currentId_;
    p.readonly          = currentReadonly_;
    p.name              = toStd(nameEdit_->text().trimmed());
    p.description       = toStd(descEdit_->text().trimmed());
    p.provider          = toStd(providerBox_->currentData().toString());
    p.model             = toStd(modelEdit_->text().trimmed());
    p.maxIter           = maxIterSpin_->value();
    p.temperature       = tempSpin_->value();
    p.showInContextMenu = showCtxCheck_->isChecked();
    p.systemPrompt      = toStd(sysPromptEdit_->toPlainText());
    p.userTemplate      = toStd(userTplEdit_->toPlainText());
    p.enabledTools      = collectEnabledTools();

    // S9：group / tags
    {
        const QString g = groupBox_->currentData().toString();
        p.group = g.isEmpty() ? std::string("general") : toStd(g);
    }
    {
        p.tags.clear();
        const QStringList raw =
            tagsEdit_->text().split(QRegularExpression(QStringLiteral("[,，;；\\s]+")),
                                    QString::SkipEmptyParts);
        for (const auto& t : raw) {
            const QString s = t.trimmed();
            if (!s.isEmpty()) p.tags.push_back(toStd(s));
        }
    }
    return p;
}

void PresetEditorDialog::setFormEnabled(bool enabled)
{
    nameEdit_->setReadOnly(!enabled);
    descEdit_->setReadOnly(!enabled);
    providerBox_->setEnabled(enabled);
    modelEdit_->setReadOnly(!enabled);
    maxIterSpin_->setReadOnly(!enabled);
    tempSpin_->setReadOnly(!enabled);
    showCtxCheck_->setEnabled(enabled);
    sysPromptEdit_->setReadOnly(!enabled);
    userTplEdit_->setReadOnly(!enabled);
    groupBox_->setEnabled(enabled);
    tagsEdit_->setReadOnly(!enabled);
    // tools 树：readonly 时整树禁用
    toolsTree_->setEnabled(enabled);
    toolsAllBtn_->setEnabled(enabled);
    toolsNoneBtn_->setEnabled(enabled);
    toolsReadOnlyBtn_->setEnabled(enabled);
    saveBtn_->setEnabled(enabled);
}

void PresetEditorDialog::markDirty(bool dirty)
{
    if (suppressDirty_) return;
    dirty_ = dirty;
}

bool PresetEditorDialog::maybeAskDiscardChanges()
{
    if (!dirty_) return true;
    auto r = QMessageBox::question(this,
        QStringLiteral("放弃修改？"),
        QStringLiteral("当前预设有未保存的修改，是否放弃？"),
        QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (r == QMessageBox::Discard) {
        dirty_ = false;
        return true;
    }
    return false;
}

void PresetEditorDialog::onNewClicked()
{
    if (!maybeAskDiscardChanges()) return;

    AgentPreset p;
    p.id          = newPresetId();
    p.name        = "新预设";
    p.description = "";
    p.systemPrompt = "You are an x64dbg reverse-engineering assistant. "
                     "Please answer in Simplified Chinese.";
    p.userTemplate = "{{user}}";
    p.maxIter      = 20;
    p.temperature  = 0.2;
    p.readonly     = false;
    p.showInContextMenu = false;

    PresetStore::instance().upsert(p);
    if (!PresetStore::instance().save()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
            QStringLiteral("写入预设文件失败，详见日志。"));
        return;
    }
    changed_ = true;
    reloadList(p.id);
}

void PresetEditorDialog::onCloneClicked()
{
    if (currentId_.empty()) return;
    if (!maybeAskDiscardChanges()) return;

    auto src = PresetStore::instance().findById(currentId_);
    if (!src) return;

    AgentPreset copy = *src;
    copy.id       = newPresetId();
    copy.name     = src->name + " (副本)";
    copy.readonly = false;
    PresetStore::instance().upsert(copy);
    if (!PresetStore::instance().save()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
            QStringLiteral("写入预设文件失败，详见日志。"));
        return;
    }
    changed_ = true;
    reloadList(copy.id);
}

void PresetEditorDialog::onDeleteClicked()
{
    if (currentId_.empty()) return;
    auto p = PresetStore::instance().findById(currentId_);
    if (!p) return;

    QString warn = QStringLiteral("确定删除预设 '%1' ?").arg(fromStd(p->name));
    if (p->readonly) {
        warn += QStringLiteral("\n\n（这是出厂预设，删除后可通过 \"恢复出厂\" 重新创建。）");
    }
    auto r = QMessageBox::question(this, QStringLiteral("删除预设"), warn,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r != QMessageBox::Yes) return;

    PresetStore::instance().remove(currentId_);
    if (!PresetStore::instance().save()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
            QStringLiteral("写入预设文件失败，详见日志。"));
        return;
    }
    changed_ = true;
    dirty_ = false;
    currentId_.clear();
    reloadList();
}

void PresetEditorDialog::onResetDefaultsClicked()
{
    auto r = QMessageBox::question(this,
        QStringLiteral("恢复出厂预设"),
        QStringLiteral("将所有出厂（🔒）预设重置为内置版本；"
                       "用户自定义预设保留。继续？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r != QMessageBox::Yes) return;

    // PresetStore::resetToDefaults 把整个列表换成默认，会丢用户预设。
    // 这里我们按"温和"语义：保留用户预设 + 用 defaults 覆盖 readonly。
    auto& store = PresetStore::instance();
    const auto defaults = defaultPresets();
    // 先收集用户预设
    std::vector<AgentPreset> userPresets;
    for (const auto& p : store.presets()) {
        if (!p.readonly) userPresets.push_back(p);
    }
    // resetToDefaults 用作"清空+灌默认"
    store.resetToDefaults();
    // 再 upsert 用户预设回去
    for (const auto& p : userPresets) store.upsert(p);
    if (!store.save()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
            QStringLiteral("写入预设文件失败，详见日志。"));
        return;
    }
    changed_ = true;
    dirty_ = false;
    reloadList(currentId_);
}

void PresetEditorDialog::onSaveCurrentClicked()
{
    if (currentId_.empty() || currentReadonly_) return;
    AgentPreset p = gatherForm();
    if (p.name.empty()) {
        QMessageBox::warning(this, QStringLiteral("校验失败"),
            QStringLiteral("名称不能为空。"));
        return;
    }
    PresetStore::instance().upsert(p);
    if (!PresetStore::instance().save()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
            QStringLiteral("写入预设文件失败，详见日志。"));
        return;
    }
    changed_ = true;
    dirty_ = false;
    XAI_LOG_INFO("preset saved: id='{}' name='{}'", p.id, p.name);

    // 刷新左侧条目文本（如果改了 name）
    if (auto* cur = listWidget_->currentItem()) {
        if (cur->childCount() == 0) {  // 仅叶子
            cur->setText(0, makeListItemLabel(p));
            cur->setToolTip(0, fromStd(p.description));
        }
    }
    // badge 也要同步
    updateBadge(p);
}

void PresetEditorDialog::onCloseClicked()
{
    if (!maybeAskDiscardChanges()) return;
    if (changed_) accept();
    else          reject();
}

void PresetEditorDialog::onToggleLockClicked()
{
    if (currentId_.empty()) return;

    auto pOpt = PresetStore::instance().findById(currentId_);
    if (!pOpt) return;

    // 若已 dirty，必须先确认是否放弃 —— 切锁需要重 populate
    if (!maybeAskDiscardChanges()) return;

    AgentPreset p = *pOpt;
    if (p.readonly) {
        // 解锁
        auto r = QMessageBox::question(this,
            QStringLiteral("解锁预设"),
            QStringLiteral("「%1」是出厂预设，默认锁定以防误改。\n\n"
                           "解锁后你对它的修改会持久化覆盖，并在下次打开时仍为已解锁状态。\n\n"
                           "确认解锁吗？")
                .arg(fromStd(p.name)),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (r != QMessageBox::Yes) return;
        p.readonly = false;
    } else {
        // 重新锁定
        auto r = QMessageBox::question(this,
            QStringLiteral("重新锁定预设"),
            QStringLiteral("将「%1」重新设为只读，之后无法直接编辑（需再次解锁）。\n\n继续？")
                .arg(fromStd(p.name)),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (r != QMessageBox::Yes) return;
        p.readonly = true;
    }

    // 持久化 readonly 翻转（其他字段保持原 store 中状态，不动表单）
    PresetStore::instance().upsert(p);
    if (!PresetStore::instance().save()) {
        QMessageBox::critical(this, QStringLiteral("失败"),
            QStringLiteral("写入预设文件失败，详见日志。"));
        return;
    }
    changed_ = true;
    XAI_LOG_INFO("preset lock toggled: id='{}' readonly={}", p.id, p.readonly);

    // 刷新左侧文本（🔒 前缀变化）+ 重 populate 表单
    reloadList(p.id);
}

}  // namespace x64ai
