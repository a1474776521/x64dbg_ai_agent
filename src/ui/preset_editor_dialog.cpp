// ui/preset_editor_dialog.cpp
#include "ui/preset_editor_dialog.h"

#include "ai/preset_store.h"
#include "ai/tools/tool_registry.h"
#include "ai/chat_provider.h"
#include "util/logging.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QToolTip>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

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
    // ---------- 左侧 ----------
    listWidget_ = new QListWidget(this);
    listWidget_->setMinimumWidth(220);

    newBtn_    = new QPushButton(QStringLiteral("新建"), this);
    cloneBtn_  = new QPushButton(QStringLiteral("复制"), this);
    deleteBtn_ = new QPushButton(QStringLiteral("删除"), this);
    resetBtn_  = new QPushButton(QStringLiteral("恢复出厂"), this);
    resetBtn_->setToolTip(QStringLiteral(
        "把所有出厂（🔒）预设重置为内置版本；用户自定义预设不受影响。"));

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

    sysPromptEdit_ = new QPlainTextEdit(this);
    sysPromptEdit_->setMinimumHeight(120);
    sysPromptEdit_->setPlaceholderText(
        QStringLiteral("role=system 初始消息。空 = 用全局默认。建议结尾追加：请用简体中文回答。"));

    userTplEdit_ = new QPlainTextEdit(this);
    userTplEdit_->setMinimumHeight(120);
    userTplEdit_->setPlaceholderText(
        QStringLiteral("用户消息模板。可用占位符：{{cip}} {{module}} {{selection}} {{disasm}} {{user}}"));

    // 工具白名单
    toolsList_ = new QListWidget(this);
    toolsList_->setMinimumHeight(120);
    toolsList_->setToolTip(QStringLiteral(
        "勾选允许 Agent 调用的工具；全部未勾选 = 等价于'全部允许'。"));
    toolsAllBtn_  = new QPushButton(QStringLiteral("全选"), this);
    toolsNoneBtn_ = new QPushButton(QStringLiteral("全清"), this);
    auto* toolsBtnRow = new QHBoxLayout();
    toolsBtnRow->setSpacing(4);
    toolsBtnRow->addStretch(1);
    toolsBtnRow->addWidget(toolsAllBtn_);
    toolsBtnRow->addWidget(toolsNoneBtn_);

    auto* toolsBox = new QGroupBox(QStringLiteral("可调用工具"), this);
    {
        auto* lay = new QVBoxLayout(toolsBox);
        lay->setContentsMargins(8, 4, 8, 8);
        lay->setSpacing(4);
        lay->addWidget(toolsList_, 1);
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
    connect(listWidget_, &QListWidget::currentItemChanged,
            this, &PresetEditorDialog::onListCurrentChanged);
    connect(newBtn_,    &QPushButton::clicked, this, &PresetEditorDialog::onNewClicked);
    connect(cloneBtn_,  &QPushButton::clicked, this, &PresetEditorDialog::onCloneClicked);
    connect(deleteBtn_, &QPushButton::clicked, this, &PresetEditorDialog::onDeleteClicked);
    connect(resetBtn_,  &QPushButton::clicked, this, &PresetEditorDialog::onResetDefaultsClicked);
    connect(saveBtn_,   &QPushButton::clicked, this, &PresetEditorDialog::onSaveCurrentClicked);
    connect(closeBtn_,  &QPushButton::clicked, this, &PresetEditorDialog::onCloseClicked);

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
    connect(toolsList_, &QListWidget::itemChanged, this,
            [this](QListWidgetItem*) { markDirty(); });

    connect(toolsAllBtn_, &QPushButton::clicked, this, [this]() {
        for (int i = 0; i < toolsList_->count(); ++i) {
            toolsList_->item(i)->setCheckState(Qt::Checked);
        }
    });
    connect(toolsNoneBtn_, &QPushButton::clicked, this, [this]() {
        for (int i = 0; i < toolsList_->count(); ++i) {
            toolsList_->item(i)->setCheckState(Qt::Unchecked);
        }
    });
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
    int selectRow = -1;
    for (int i = 0; i < static_cast<int>(all.size()); ++i) {
        const auto& p = all[i];
        auto* it = new QListWidgetItem(makeListItemLabel(p), listWidget_);
        it->setData(Qt::UserRole, fromStd(p.id));
        if (!p.description.empty()) it->setToolTip(fromStd(p.description));
        if (!selectId.empty() && p.id == selectId) selectRow = i;
    }
    listWidget_->blockSignals(false);

    if (listWidget_->count() == 0) {
        currentId_.clear();
        currentReadonly_ = false;
        setFormEnabled(false);
        return;
    }
    if (selectRow < 0) selectRow = 0;
    listWidget_->setCurrentRow(selectRow);
}

void PresetEditorDialog::onListCurrentChanged()
{
    if (!maybeAskDiscardChanges()) {
        // 用户取消 → 还原选中项
        // 找到 currentId_ 对应行
        for (int i = 0; i < listWidget_->count(); ++i) {
            if (listWidget_->item(i)->data(Qt::UserRole).toString().toStdString() == currentId_) {
                listWidget_->blockSignals(true);
                listWidget_->setCurrentRow(i);
                listWidget_->blockSignals(false);
                return;
            }
        }
        return;
    }
    auto* cur = listWidget_->currentItem();
    if (!cur) { setFormEnabled(false); return; }
    std::string id = cur->data(Qt::UserRole).toString().toStdString();
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
    loadEnabledTools(p.enabledTools);

    setFormEnabled(!p.readonly);
    deleteBtn_->setEnabled(true);  // 用户要求：readonly 也允许删
    saveBtn_->setEnabled(!p.readonly);
    suppressDirty_ = false;
    dirty_ = false;
}

void PresetEditorDialog::loadEnabledTools(const std::vector<std::string>& enabled)
{
    // 1) 拉取全部已注册工具
    auto chatTools = ToolRegistry::instance().listChatTools();
    std::sort(chatTools.begin(), chatTools.end(),
              [](const ChatTool& a, const ChatTool& b) { return a.name < b.name; });

    std::set<std::string> enabledSet(enabled.begin(), enabled.end());
    const bool selectAll = enabled.empty();  // 约定：空集 = 全部

    toolsList_->blockSignals(true);
    toolsList_->clear();
    for (const auto& t : chatTools) {
        auto* it = new QListWidgetItem(toolsList_);
        it->setText(fromStd(t.name));
        it->setToolTip(fromStd(t.description));
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        const bool on = selectAll || enabledSet.count(t.name) > 0;
        it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
    toolsList_->blockSignals(false);
}

std::vector<std::string> PresetEditorDialog::collectEnabledTools() const
{
    std::vector<std::string> out;
    const int n = toolsList_->count();
    int checked = 0;
    for (int i = 0; i < n; ++i) {
        if (toolsList_->item(i)->checkState() == Qt::Checked) ++checked;
    }
    // 全部勾选 = 等价于"留空"，统一规范化为空集
    if (checked == n) return out;
    for (int i = 0; i < n; ++i) {
        if (toolsList_->item(i)->checkState() == Qt::Checked) {
            out.push_back(toStd(toolsList_->item(i)->text()));
        }
    }
    return out;
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
    // tools 复选：readonly 时也禁止勾选
    for (int i = 0; i < toolsList_->count(); ++i) {
        auto* it = toolsList_->item(i);
        if (enabled) it->setFlags(it->flags() |  Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        else         it->setFlags((it->flags() & ~Qt::ItemIsEnabled));
    }
    toolsAllBtn_->setEnabled(enabled);
    toolsNoneBtn_->setEnabled(enabled);
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
    int row = listWidget_->currentRow();
    if (row >= 0) {
        listWidget_->item(row)->setText(makeListItemLabel(p));
        listWidget_->item(row)->setToolTip(fromStd(p.description));
    }
}

void PresetEditorDialog::onCloseClicked()
{
    if (!maybeAskDiscardChanges()) return;
    if (changed_) accept();
    else          reject();
}

}  // namespace x64ai
