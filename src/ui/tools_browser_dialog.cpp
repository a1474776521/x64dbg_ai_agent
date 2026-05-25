// ui/tools_browser_dialog.cpp
#include "ui/tools_browser_dialog.h"
#include "ui/preset_editor_dialog.h"   // for prettyGroupName

#include "ai/tools/tool_registry.h"
#include "ai/agent_preset.h"
#include "ai/preset_store.h"

#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QDialogButtonBox>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace x64ai {

namespace {

inline QString fromStd(const std::string& s) { return QString::fromUtf8(s.c_str(), int(s.size())); }

constexpr int kRoleIsGroup   = Qt::UserRole;       // bool 标记
constexpr int kRoleToolName  = Qt::UserRole + 1;   // 叶子：工具名
constexpr int kRoleCategory  = Qt::UserRole + 2;   // 叶子：ToolCategory int
constexpr int kRoleEnabled   = Qt::UserRole + 3;   // 叶子：是否被预设启用

}  // namespace

ToolsBrowserDialog::ToolsBrowserDialog(QWidget* parent, const std::string& activePresetId)
    : QDialog(parent),
      activePresetId_(activePresetId),
      noActivePreset_(activePresetId.empty())
{
    setWindowTitle(QStringLiteral("已注册工具一览"));
    resize(960, 640);

    // 拉取启用集
    if (!activePresetId_.empty()) {
        PresetStore::instance().load();
        if (auto p = PresetStore::instance().findById(activePresetId_)) {
            enabledSet_ = p->enabledTools;
        }
    }

    buildUi();
    buildTree();
    applyFilter();
    updateBadge();
}

void ToolsBrowserDialog::buildUi()
{
    // ---- 顶部 badge ----
    badgeLabel_ = new QLabel(this);
    badgeLabel_->setStyleSheet(QStringLiteral(
        "QLabel { background:#2D2F33; color:#B7C0CC; padding:6px 10px; "
        "border-radius:4px; font-size:11px; }"));
    badgeLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    badgeLabel_->setWordWrap(true);

    // ---- 过滤行 ----
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(QStringLiteral("搜索工具名 / 描述..."));
    searchEdit_->setClearButtonEnabled(true);

    chipRead_  = new QCheckBox(QStringLiteral("Read"),  this);
    chipCtrl_  = new QCheckBox(QStringLiteral("Ctrl"),  this);
    chipWrite_ = new QCheckBox(QStringLiteral("Write"), this);
    chipRead_->setChecked(true);
    chipCtrl_->setChecked(true);
    chipWrite_->setChecked(true);
    chipRead_->setToolTip(QStringLiteral("勾选以显示只读工具，取消则隐藏"));
    chipCtrl_->setToolTip(QStringLiteral("勾选以显示调试控制工具，取消则隐藏"));
    chipWrite_->setToolTip(QStringLiteral("勾选以显示写工具，取消则隐藏"));

    chipOnlyActive_ = new QCheckBox(QStringLiteral("只显示当前预设启用"), this);
    // 默认不勾选 = 显示全部；勾选 = 仅显示当前预设 enabledTools 集合内的工具
    chipOnlyActive_->setChecked(false);
    chipOnlyActive_->setEnabled(!noActivePreset_);
    chipOnlyActive_->setToolTip(noActivePreset_
        ? QStringLiteral("当前未选择激活预设，无法过滤")
        : (enabledSet_.empty()
              ? QStringLiteral("当前预设 enabledTools=空（语义=全部启用），勾选与否结果相同")
              : QStringLiteral("勾选后仅显示当前激活预设 enabledTools 集合内的工具")));

    auto* filterRow = new QHBoxLayout();
    filterRow->setSpacing(8);
    filterRow->addWidget(searchEdit_, 1);
    filterRow->addWidget(chipRead_);
    filterRow->addWidget(chipCtrl_);
    filterRow->addWidget(chipWrite_);
    filterRow->addSpacing(8);
    filterRow->addWidget(chipOnlyActive_);

    // ---- 主树 ----
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(4);
    tree_->setHeaderLabels({
        QStringLiteral("工具 / 分组"),
        QStringLiteral("类别"),
        QStringLiteral("描述"),
        QStringLiteral("启用"),
    });
    tree_->setRootIsDecorated(true);
    tree_->setExpandsOnDoubleClick(false);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(false);
    tree_->setWordWrap(true);
    tree_->setTextElideMode(Qt::ElideNone);
    tree_->setSortingEnabled(true);
    tree_->setHeaderHidden(false);

    auto* hh = tree_->header();
    hh->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(2, QHeaderView::Stretch);
    hh->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    hh->setSectionsClickable(true);
    hh->setStretchLastSection(false);
    hh->setToolTip(QStringLiteral("点击列头排序；双击行查看完整 schema"));

    // ---- 底部按钮 ----
    openEditorBtn_ = new QPushButton(QStringLiteral("打开工作流编辑器…"), this);
    openEditorBtn_->setToolTip(QStringLiteral(
        "想修改「当前工作流到底启用哪些工具」请去工作流编辑器"));
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    bb->addButton(openEditorBtn_, QDialogButtonBox::ActionRole);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(openEditorBtn_, &QPushButton::clicked, this, &ToolsBrowserDialog::onOpenPresetEditor);

    // ---- 装配 ----
    auto* lay = new QVBoxLayout(this);
    lay->setSpacing(8);
    lay->addWidget(badgeLabel_);
    lay->addLayout(filterRow);
    lay->addWidget(tree_, 1);
    lay->addWidget(bb);

    // ---- 信号 ----
    connect(searchEdit_, &QLineEdit::textChanged, this, &ToolsBrowserDialog::onFilterChanged);
    connect(chipRead_,  &QCheckBox::toggled, this, &ToolsBrowserDialog::onFilterChanged);
    connect(chipCtrl_,  &QCheckBox::toggled, this, &ToolsBrowserDialog::onFilterChanged);
    connect(chipWrite_, &QCheckBox::toggled, this, &ToolsBrowserDialog::onFilterChanged);
    connect(chipOnlyActive_, &QCheckBox::toggled, this, &ToolsBrowserDialog::onFilterChanged);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, &ToolsBrowserDialog::onItemDoubleClicked);
}

void ToolsBrowserDialog::buildTree()
{
    tree_->setSortingEnabled(false);
    tree_->clear();

    auto& reg = ToolRegistry::instance();
    auto chatTools = reg.listChatTools();

    // group → vector<ChatTool*>
    std::map<std::string, std::vector<const ChatTool*>> byGroup;
    for (const auto& t : chatTools) {
        byGroup[reg.groupOf(t.name)].push_back(&t);
    }

    static const std::vector<std::string> kOrder = {
        "static-info", "disasm-cfg", "memory-search", "register-stack",
        "breakpoint", "execution-control", "annotation", "write-patch",
        "gui-misc", "agent-meta", "anti-debug-insight", "forensics",
    };
    std::vector<std::string> order;
    for (const auto& g : kOrder) if (byGroup.count(g)) order.push_back(g);
    for (const auto& [g, _] : byGroup)
        if (std::find(order.begin(), order.end(), g) == order.end()) order.push_back(g);

    auto catColors = [](ToolCategory c) -> std::pair<QColor, QColor> {
        switch (c) {
            case ToolCategory::Read:       return { QColor(0x6F, 0xCF, 0x97), QColor(0x1B, 0x3A, 0x2A) };
            case ToolCategory::DbgControl: return { QColor(0xF2, 0xC8, 0x4B), QColor(0x3D, 0x33, 0x12) };
            case ToolCategory::Write:      return { QColor(0xEB, 0x5C, 0x5C), QColor(0x40, 0x1F, 0x1F) };
        }
        return { QColor(0xB7, 0xC0, 0xCC), QColor() };
    };

    const std::set<std::string> enabledSetLookup(enabledSet_.begin(), enabledSet_.end());
    const bool allEnabled = noActivePreset_ || enabledSet_.empty();

    for (const auto& g : order) {
        auto& list = byGroup[g];
        std::sort(list.begin(), list.end(),
                  [](const ChatTool* a, const ChatTool* b) { return a->name < b->name; });

        auto* grp = new QTreeWidgetItem(tree_);
        grp->setText(0, QStringLiteral("%1   [%2]")
                          .arg(PresetEditorDialog::prettyGroupName(g))
                          .arg(list.size()));
        grp->setData(0, kRoleIsGroup, true);
        grp->setFirstColumnSpanned(false);
        QFont f = grp->font(0); f.setBold(true); grp->setFont(0, f);
        grp->setExpanded(true);

        int grpEnabled = 0;
        for (const auto* t : list) {
            auto* leaf = new QTreeWidgetItem(grp);
            const ToolCategory cat = reg.categoryOf(t->name);
            const char* catTxt =
                cat == ToolCategory::Read       ? "Read"  :
                cat == ToolCategory::DbgControl ? "Ctrl"  :
                cat == ToolCategory::Write      ? "Write" : "?";
            const bool isEnabled = allEnabled || enabledSetLookup.count(t->name) > 0;
            if (isEnabled) ++grpEnabled;

            // 描述：优先中文，空则 fall back 英文
            const std::string& descShow = !t->descriptionZh.empty() ? t->descriptionZh : t->description;

            leaf->setText(0, fromStd(t->name));
            leaf->setText(1, QString::fromLatin1(catTxt));
            leaf->setText(2, fromStd(descShow));
            leaf->setText(3, isEnabled ? QStringLiteral("✓") : QStringLiteral("✗"));
            leaf->setToolTip(0, fromStd(descShow));
            leaf->setToolTip(1, QStringLiteral(
                "Read：只读，不改状态\n"
                "Ctrl：控制运行/断点等可逆操作\n"
                "Write：写内存/寄存器/补丁（高风险）"));
            leaf->setToolTip(2, fromStd(descShow));
            leaf->setToolTip(3, allEnabled
                ? (noActivePreset_
                      ? QStringLiteral("无激活预设：所有工具均可调用")
                      : QStringLiteral("当前预设 enabledTools=空 → 全部启用"))
                : (isEnabled
                      ? QStringLiteral("当前预设已启用此工具")
                      : QStringLiteral("当前预设未启用此工具")));

            leaf->setData(0, kRoleIsGroup,   false);
            leaf->setData(0, kRoleToolName,  fromStd(t->name));
            leaf->setData(0, kRoleCategory,  static_cast<int>(cat));
            leaf->setData(0, kRoleEnabled,   isEnabled);

            // 类别列彩色徽标 + 居中加粗
            auto [fg, bg] = catColors(cat);
            leaf->setForeground(1, QBrush(fg));
            if (bg.isValid()) leaf->setBackground(1, QBrush(bg));
            leaf->setTextAlignment(1, Qt::AlignCenter);
            QFont lf = leaf->font(1); lf.setBold(true); leaf->setFont(1, lf);

            // 启用列居中；✓ 绿、✗ 灰
            leaf->setTextAlignment(3, Qt::AlignCenter);
            leaf->setForeground(3, QBrush(isEnabled
                ? QColor(0x6F, 0xCF, 0x97) : QColor(0x6A, 0x6A, 0x6A)));
            QFont ef = leaf->font(3); ef.setBold(true); leaf->setFont(3, ef);
        }
        // 组节点末尾标启用数（如 "静态信息 (static-info)   [5]  · 启用 3"）
        if (!allEnabled) {
            grp->setText(0, QStringLiteral("%1   [%2]   · 启用 %3")
                              .arg(PresetEditorDialog::prettyGroupName(g))
                              .arg(list.size())
                              .arg(grpEnabled));
        }
    }
    tree_->setSortingEnabled(true);
}

void ToolsBrowserDialog::applyFilter()
{
    const QString needle = searchEdit_ ? searchEdit_->text().trimmed().toLower() : QString();
    const bool wantRead  = chipRead_  && chipRead_->isChecked();
    const bool wantCtrl  = chipCtrl_  && chipCtrl_->isChecked();
    const bool wantWrite = chipWrite_ && chipWrite_->isChecked();
    const bool onlyActive = chipOnlyActive_ && chipOnlyActive_->isChecked();

    int visTotal = 0, visEnabled = 0;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* grp = tree_->topLevelItem(i);
        int grpVis = 0;
        for (int j = 0; j < grp->childCount(); ++j) {
            auto* leaf = grp->child(j);
            const int cat       = leaf->data(0, kRoleCategory).toInt();
            const bool isEnabled = leaf->data(0, kRoleEnabled).toBool();
            const QString name   = leaf->data(0, kRoleToolName).toString().toLower();
            const QString desc   = leaf->text(2).toLower();   // 已经是 descShow（中文优先）

            bool ok = true;
            switch (static_cast<ToolCategory>(cat)) {
                case ToolCategory::Read:       if (!wantRead)  ok = false; break;
                case ToolCategory::DbgControl: if (!wantCtrl)  ok = false; break;
                case ToolCategory::Write:      if (!wantWrite) ok = false; break;
            }
            if (ok && onlyActive && !isEnabled) ok = false;
            if (ok && !needle.isEmpty() && !name.contains(needle) && !desc.contains(needle)) ok = false;

            leaf->setHidden(!ok);
            if (ok) {
                ++grpVis;
                ++visTotal;
                if (isEnabled) ++visEnabled;
            }
        }
        grp->setHidden(grpVis == 0);
    }
    // 更新 badge 末尾的过滤计数
    if (badgeLabel_) {
        QString cur = badgeLabel_->property("baseText").toString();
        if (cur.isEmpty()) cur = badgeLabel_->text();
        badgeLabel_->setText(cur + QStringLiteral("    · 过滤后可见 %1（其中启用 %2）")
                                     .arg(visTotal).arg(visEnabled));
    }
}

void ToolsBrowserDialog::updateBadge()
{
    if (!badgeLabel_) return;
    auto& reg = ToolRegistry::instance();
    const int total = static_cast<int>(reg.listChatTools().size());

    QString head;
    if (noActivePreset_) {
        head = QStringLiteral("共 %1 个已注册工具  ·  当前无激活预设：全部可调用").arg(total);
    } else if (enabledSet_.empty()) {
        head = QStringLiteral("共 %1 个已注册工具  ·  当前预设 enabledTools=空 → 全部启用").arg(total);
    } else {
        head = QStringLiteral("共 %1 个已注册工具  ·  当前预设启用 %2 个")
                   .arg(total).arg(static_cast<int>(enabledSet_.size()));
    }
    badgeLabel_->setProperty("baseText", head);
    badgeLabel_->setText(head);
    applyFilter();   // 让"过滤后可见 N"立刻刷出来
}

void ToolsBrowserDialog::onFilterChanged()
{
    applyFilter();
}

void ToolsBrowserDialog::onItemDoubleClicked(QTreeWidgetItem* item, int /*column*/)
{
    if (!item) return;
    if (item->data(0, kRoleIsGroup).toBool()) {
        item->setExpanded(!item->isExpanded());
        return;
    }
    showToolDetails(item);
}

void ToolsBrowserDialog::onOpenPresetEditor()
{
    openPresetEditor_ = true;
    accept();   // 关闭本对话框；调用方据 openPresetEditorRequested() 跳转
}

void ToolsBrowserDialog::showToolDetails(QTreeWidgetItem* leaf)
{
    if (!leaf) return;
    const std::string name = leaf->data(0, kRoleToolName).toString().toStdString();
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
    const QString group = PresetEditorDialog::prettyGroupName(reg.groupOf(name));
    const bool isEnabled = leaf->data(0, kRoleEnabled).toBool();
    const QString enabledTxt = isEnabled
        ? QStringLiteral("<span style='color:#6FCF97'>✓ 启用</span>")
        : QStringLiteral("<span style='color:#EB5C5C'>✗ 未启用</span>");

    QString prettySchema = fromStd(meta->parametersJson);
    try {
        auto j = nlohmann::json::parse(meta->parametersJson);
        prettySchema = fromStd(j.dump(2));
    } catch (...) {}

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("工具详情 - %1").arg(fromStd(name)));
    dlg.resize(680, 560);
    dlg.setStyleSheet(this->styleSheet());   // 继承父对话框 QSS

    auto* header = new QLabel(&dlg);
    header->setTextFormat(Qt::RichText);
    header->setWordWrap(true);
    header->setText(QStringLiteral(
        "<table cellspacing='6'>"
        "<tr><td><b>名称</b></td><td><code>%1</code></td></tr>"
        "<tr><td><b>分组</b></td><td>%2</td></tr>"
        "<tr><td><b>类别</b></td><td>%3</td></tr>"
        "<tr><td><b>当前预设</b></td><td>%4</td></tr>"
        "<tr><td valign='top'><b>描述</b></td><td>%5</td></tr>"
        "</table>")
        .arg(fromStd(name).toHtmlEscaped())
        .arg(group.toHtmlEscaped())
        .arg(catTxt.toHtmlEscaped())
        .arg(enabledTxt)
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

}  // namespace x64ai
