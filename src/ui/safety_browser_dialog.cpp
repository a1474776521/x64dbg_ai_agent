// ui/safety_browser_dialog.cpp
#include "ui/safety_browser_dialog.h"

#include "ai/tools/bp_safety.h"
#include "ai/tools/confirm_policy.h"
#include "ai/tools/tool_registry.h"
#include "util/config.h"
#include "util/paths.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

namespace x64ai {

namespace {

inline QString fromStd(const std::string& s) { return QString::fromUtf8(s.c_str(), int(s.size())); }

std::vector<std::string> sortedCopy(const std::unordered_set<std::string>& s)
{
    std::vector<std::string> v(s.begin(), s.end());
    std::sort(v.begin(), v.end());
    return v;
}

// 公共：构建一个带"搜索 + 计数 + 简表"的面板
QWidget* makeSearchableList(const std::vector<std::string>& items,
                            const std::vector<std::string>& tags,  // 与 items 等长；空字符串表无标签
                            const QString& headerColName,
                            const QString& topNoteRich)
{
    auto* w = new QWidget;
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    auto* note = new QLabel(topNoteRich);
    note->setTextFormat(Qt::RichText);
    note->setWordWrap(true);
    note->setOpenExternalLinks(false);
    note->setStyleSheet(QStringLiteral(
        "QLabel { background:#2D2F33; color:#B7C0CC; padding:8px 10px; "
        "border-radius:4px; font-size:11px; }"));
    lay->addWidget(note);

    auto* searchRow = new QHBoxLayout;
    auto* search = new QLineEdit;
    search->setPlaceholderText(QStringLiteral("搜索…"));
    search->setClearButtonEnabled(true);
    auto* count = new QLabel(QStringLiteral("共 %1 条").arg(int(items.size())));
    searchRow->addWidget(search, 1);
    searchRow->addWidget(count);
    lay->addLayout(searchRow);

    const bool hasTags = !tags.empty();
    auto* tbl = new QTableWidget;
    tbl->setColumnCount(hasTags ? 2 : 1);
    if (hasTags) {
        tbl->setHorizontalHeaderLabels({ headerColName, QStringLiteral("来源 / 备注") });
    } else {
        tbl->setHorizontalHeaderLabels({ headerColName });
    }
    tbl->setRowCount(int(items.size()));
    tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    tbl->setAlternatingRowColors(true);
    tbl->verticalHeader()->setVisible(false);
    tbl->horizontalHeader()->setStretchLastSection(true);
    tbl->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    for (int i = 0; i < int(items.size()); ++i) {
        auto* it = new QTableWidgetItem(fromStd(items[i]));
        it->setToolTip(QStringLiteral("双击复制"));
        tbl->setItem(i, 0, it);
        if (hasTags) {
            auto* tag = new QTableWidgetItem(fromStd(tags[i]));
            tbl->setItem(i, 1, tag);
        }
    }
    tbl->setSortingEnabled(true);
    lay->addWidget(tbl, 1);

    QObject::connect(tbl, &QTableWidget::itemDoubleClicked, tbl, [](QTableWidgetItem* it){
        if (it) QGuiApplication::clipboard()->setText(it->text());
    });

    QObject::connect(search, &QLineEdit::textChanged, tbl, [tbl, count](const QString& q){
        const QString needle = q.trimmed().toLower();
        int visible = 0;
        for (int r = 0; r < tbl->rowCount(); ++r) {
            bool match = needle.isEmpty();
            if (!match) {
                for (int c = 0; c < tbl->columnCount(); ++c) {
                    auto* it = tbl->item(r, c);
                    if (it && it->text().toLower().contains(needle)) { match = true; break; }
                }
            }
            tbl->setRowHidden(r, !match);
            if (match) ++visible;
        }
        count->setText(QStringLiteral("匹配 %1 / 共 %2 条").arg(visible).arg(tbl->rowCount()));
    });

    return w;
}

}  // namespace

SafetyBrowserDialog::SafetyBrowserDialog(QWidget* parent, InitialTab initial)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("x64aiSafetyBrowser"));
    setWindowTitle(QStringLiteral("安全护栏 - 白名单 / 黑名单一览"));
    resize(880, 620);

    // Dark QSS（限定在本对话框 objectName 子树，不污染 x64dbg 其他窗口）。
    // 修复：之前 QTableWidget header / cell 在系统浅色 palette 下出现"白底灰字"看不清。
    setStyleSheet(QStringLiteral(R"(
        #x64aiSafetyBrowser { background:#202225; color:#E6E8EB; }
        #x64aiSafetyBrowser QLabel { color:#E6E8EB; }
        #x64aiSafetyBrowser QTabWidget::pane {
            background:#202225; border:1px solid #3A3D42; top:-1px;
        }
        #x64aiSafetyBrowser QTabBar::tab {
            background:#2A2C30; color:#B7C0CC; padding:6px 14px;
            border:1px solid #3A3D42; border-bottom:none;
            border-top-left-radius:3px; border-top-right-radius:3px;
        }
        #x64aiSafetyBrowser QTabBar::tab:selected {
            background:#202225; color:#E6E8EB;
        }
        #x64aiSafetyBrowser QTabBar::tab:hover { background:#34373C; }

        #x64aiSafetyBrowser QLineEdit, #x64aiSafetyBrowser QPlainTextEdit {
            background:#1B1D20; color:#E6E8EB;
            border:1px solid #3A3D42; border-radius:3px; padding:4px 6px;
            selection-background-color:#3B82F6; selection-color:#FFFFFF;
        }
        #x64aiSafetyBrowser QPlainTextEdit { padding:6px; }

        #x64aiSafetyBrowser QPushButton {
            background:#2D2F33; color:#E6E8EB;
            border:1px solid #3A3D42; border-radius:3px; padding:5px 12px;
        }
        #x64aiSafetyBrowser QPushButton:hover { background:#3A3D42; }
        #x64aiSafetyBrowser QPushButton:pressed { background:#1F2125; }
        #x64aiSafetyBrowser QPushButton:default { border:1px solid #3B82F6; }

        /* 关键修复：表头 + 表格内容统一 dark */
        #x64aiSafetyBrowser QHeaderView::section {
            background:#2D2F33; color:#E6E8EB;
            border:none; border-right:1px solid #3A3D42; border-bottom:1px solid #3A3D42;
            padding:6px 8px; font-weight:600;
        }
        #x64aiSafetyBrowser QHeaderView { background:#2D2F33; }
        #x64aiSafetyBrowser QTableCornerButton::section {
            background:#2D2F33; border:1px solid #3A3D42;
        }
        #x64aiSafetyBrowser QTableView, #x64aiSafetyBrowser QTableWidget {
            background:#1B1D20; color:#E6E8EB;
            alternate-background-color:#23262A;
            gridline-color:#33363B;
            selection-background-color:#3B82F6; selection-color:#FFFFFF;
            border:1px solid #3A3D42;
        }
        #x64aiSafetyBrowser QTableView::item, #x64aiSafetyBrowser QTableWidget::item {
            color:#E6E8EB; padding:3px 6px;
        }
        #x64aiSafetyBrowser QTableView::item:selected,
        #x64aiSafetyBrowser QTableWidget::item:selected {
            background:#3B82F6; color:#FFFFFF;
        }

        /* 滚动条 */
        #x64aiSafetyBrowser QScrollBar:vertical {
            background:#202225; width:12px; margin:0;
        }
        #x64aiSafetyBrowser QScrollBar::handle:vertical {
            background:#3A3D42; min-height:24px; border-radius:3px;
        }
        #x64aiSafetyBrowser QScrollBar::handle:vertical:hover { background:#4A4D52; }
        #x64aiSafetyBrowser QScrollBar::add-line:vertical,
        #x64aiSafetyBrowser QScrollBar::sub-line:vertical { height:0; }
        #x64aiSafetyBrowser QScrollBar:horizontal {
            background:#202225; height:12px; margin:0;
        }
        #x64aiSafetyBrowser QScrollBar::handle:horizontal {
            background:#3A3D42; min-width:24px; border-radius:3px;
        }
        #x64aiSafetyBrowser QScrollBar::handle:horizontal:hover { background:#4A4D52; }
        #x64aiSafetyBrowser QScrollBar::add-line:horizontal,
        #x64aiSafetyBrowser QScrollBar::sub-line:horizontal { width:0; }
    )"));

    buildUi(initial);
}

void SafetyBrowserDialog::buildUi(InitialTab initial)
{
    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildWhitelistTab(),    QStringLiteral("run_dbg_command 白名单"));
    tabs_->addTab(buildSysModulesTab(),   QStringLiteral("系统模块黑名单 (K-30)"));
    tabs_->addTab(buildHotApisTab(),      QStringLiteral("高频 API 黑名单 (K-30)"));
    tabs_->addTab(buildAutoApproveTab(),  QStringLiteral("Confirm 豁免 (K-33)"));
    tabs_->addTab(buildHowToTab(),        QStringLiteral("如何在 config.json 配置"));
    tabs_->setCurrentIndex(int(initial));

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(tabs_, 1);
    lay->addWidget(bb);
}

QWidget* SafetyBrowserDialog::buildWhitelistTab()
{
    const auto& defaults = dbgCmdWhitelistDefaults();
    const auto& userExtra = Config::instance().get().extraDbgCmdWhitelist;

    // 合并展示，加来源标签；默认在前，用户追加在后；去重时以默认优先
    std::vector<std::string> items;
    std::vector<std::string> tags;
    auto sortedDefaults = sortedCopy(defaults);
    for (const auto& s : sortedDefaults) {
        items.push_back(s);
        tags.push_back("默认 (代码硬编码)");
    }
    std::unordered_set<std::string> defLookup(defaults.begin(), defaults.end());
    std::vector<std::string> extraSorted(userExtra.begin(), userExtra.end());
    std::sort(extraSorted.begin(), extraSorted.end());
    int userCount = 0;
    for (const auto& s : extraSorted) {
        if (defLookup.count(s)) continue;  // 用户重复加默认项也只算默认
        items.push_back(s);
        tags.push_back("用户配置 (config.json)");
        ++userCount;
    }

    const QString note = QStringLiteral(
        "<b>run_dbg_command</b> 工具的命令首 token 白名单。LLM 调 <code>run_dbg_command(\"bp 0x401000\")</code> "
        "时，<code>bp</code> 必须在此名单内才会执行；否则直接拒绝。<br/>"
        "默认 <b>%1</b> 条 + 用户配置追加 <b>%2</b> 条 = 合并 <b>%3</b> 条。"
        "<br/>用户配置见 \"如何在 config.json 配置\" 标签页。"
        "<br/><span style='color:#F2C84B'>注意：</span>用户配置只能<b>追加</b>，不能从默认集移除（避免误关安全护栏）。"
        ).arg(int(defaults.size())).arg(userCount).arg(int(items.size()));

    return makeSearchableList(items, tags, QStringLiteral("命令首 token (小写)"), note);
}

QWidget* SafetyBrowserDialog::buildSysModulesTab()
{
    auto items = sortedCopy(bpSysModules());
    const QString note = QStringLiteral(
        "K-30 系统模块黑名单。在这些模块（如 ntdll/kernel32/user32 ...）的<b>高频 API</b>（见下一标签页）"
        "上下 execute 类型断点，会让被调试进程被频繁暂停 → 全局键鼠 hook 失效 → <b>整个桌面卡死</b>。<br/>"
        "由 <code>set_breakpoint</code> 和 <code>set_hw_breakpoint</code> 两个工具检查：<b>系统模块 ∧ 高频 API → 拒绝</b>。<br/>"
        "只是<b>系统模块</b>但不在高频 API 名单内的位置仍可下断（如 <code>ntdll.RtlInitUnicodeString</code> 不在 hot list）。"
        );
    return makeSearchableList(items, {}, QStringLiteral("模块名 (无扩展，小写)"), note);
}

QWidget* SafetyBrowserDialog::buildHotApisTab()
{
    auto items = sortedCopy(bpHotApis());
    const QString note = QStringLiteral(
        "K-30 高频 API 黑名单。这些 API 在被调试进程里调用频率极高（每秒数十到数百次），"
        "下断会让进程几乎每秒暂停几十次。<br/>"
        "判定为危险的条件是：<b>调用地址所在模块在系统模块黑名单</b> ∧ <b>符号名在此名单</b>。<br/>"
        "如果你确实需要观察这些 API，请用："
        "<ol>"
        "<li><code>set_conditional_bp</code> 带过滤条件 (如 arg.get(0) == 某值)，让 x64dbg 内核自动过滤大部分不感兴趣的命中；</li>"
        "<li>在<b>调用方</b>用户代码下断（用 <code>find_xrefs_to</code> / <code>locate_api_callers</code> 找出调用 LoadLibraryW 的那条 call 指令再下断）。</li>"
        "</ol>"
        );
    return makeSearchableList(items, {}, QStringLiteral("API 符号名 (小写)"), note);
}

QWidget* SafetyBrowserDialog::buildAutoApproveTab()
{
    // 收集：黑名单（强制 confirm 永不豁免） + 当前用户配置生效的豁免集 + 所有写工具列表
    const auto& hard       = confirmHardEnforced();
    const auto& userAuto   = autoApproveTools();

    // 拉取所有 Write 工具名，按"豁免状态"做来源标签
    // 注意：ToolRegistry 没有暴露 ITool*，无法读 requiresUserConfirmation()；
    // 直接列 ToolCategory::Write 的全集。少数 Write 工具其实不弹 confirm（如 patch_misc 内 4 个），
    // 这些工具加进 auto_approve_tools 是 no-op，不影响安全语义。
    std::vector<std::string> writeNames;
    {
        const auto& reg = ToolRegistry::instance();
        for (const auto& ct : reg.listChatTools()) {
            if (reg.categoryOf(ct.name) != ToolCategory::Write) continue;
            writeNames.push_back(ct.name);
        }
        std::sort(writeNames.begin(), writeNames.end());
    }

    auto lower = [](std::string s){
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    std::vector<std::string> items;
    std::vector<std::string> tags;
    int hardCount = 0, autoCount = 0, normalCount = 0;
    for (const auto& n : writeNames) {
        const auto nl = lower(n);
        items.push_back(n);
        if (hard.count(nl)) {
            tags.push_back("强制 confirm (黑名单 · 不可豁免)");
            ++hardCount;
        } else if (userAuto.count(nl)) {
            tags.push_back("✅ 已豁免 (用户配置)");
            ++autoCount;
        } else {
            tags.push_back("默认弹 5s confirm");
            ++normalCount;
        }
    }

    const QString note = QStringLiteral(
        "<b>K-33</b>：用户可在 <code>config.json</code> 配置 <code>auto_approve_tools</code> 让指定写工具<b>跳过 5s 确认弹窗</b>。"
        "豁免的工具<b>仍写 audit log</b>（phase=auto_approved）。<br/>"
        "<br/>当前状态："
        "<b>强制 confirm</b> <span style='color:#FF6B6B'>%1</span> 项 · "
        "<b>✅ 已豁免</b> <span style='color:#7AC74F'>%2</span> 项 · "
        "<b>默认弹窗</b> <span style='color:#B7C0CC'>%3</span> 项"
        "<br/><br/><b>黑名单</b>（即使写进 auto_approve_tools 也无效，避免误关键护栏）："
        "<code>%4</code>"
        ).arg(hardCount).arg(autoCount).arg(normalCount)
         .arg(QString::fromStdString([&]{
             std::string s;
             for (const auto& n : hard) { if (!s.empty()) s += ", "; s += n; }
             return s;
         }()));

    return makeSearchableList(items, tags, QStringLiteral("写工具名"), note);
}

QWidget* SafetyBrowserDialog::buildHowToTab()
{
    auto* w = new QWidget;
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    namespace fs = std::filesystem;
    const auto cfgPath = pluginConfigFile();
    const QString cfgPathStr = fromStd(cfgPath.string());
    const bool cfgExists = fs::exists(cfgPath);

    auto* head = new QLabel(QStringLiteral(
        "<h3 style='margin:0'>config.json 可配置字段</h3>"
        "<p>编辑 <code>%1</code>（不存在则新建），加入以下字段：</p>"
        "<ul>"
        "<li><code>extra_dbg_cmd_whitelist</code>：追加 run_dbg_command 白名单（默认 13 项硬集，见 Tab1）</li>"
        "<li><code>auto_approve_tools</code>：豁免指定写工具的 5s confirm 弹窗，K-33 新增（仍写 audit；黑名单工具不可豁免，见 Tab4）</li>"
        "</ul>")
        .arg(cfgPathStr));
    head->setTextFormat(Qt::RichText);
    head->setWordWrap(true);
    lay->addWidget(head);

    auto* sample = new QPlainTextEdit;
    sample->setPlainText(QStringLiteral(R"json({
  "provider": "deepseek",
  "default_model": "deepseek-chat",

  "extra_dbg_cmd_whitelist": [
    "bpdll",
    "bcdll",
    "InitDebug",
    "StopDebug"
  ],

  "auto_approve_tools": [
    "set_label",
    "set_comment",
    "add_function",
    "remove_breakpoint",
    "remove_hw_breakpoint",
    "restore_patch",
    "set_flag",
    "set_conditional_bp"
  ]
})json"));
    sample->setReadOnly(true);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    sample->setFont(mono);
    sample->setMaximumHeight(280);
    lay->addWidget(sample);

    auto* notes = new QLabel(QStringLiteral(
        "<ul>"
        "<li>白名单命令名按 x64dbg 命令首 token，大小写不敏感（内部统一转小写）。</li>"
        "<li>豁免工具名按 ITool::name()，与工具列表展示一致，大小写不敏感。</li>"
        "<li><b>修改后必须重启 x64dbg</b>，合并集只在插件启动时构建一次。</li>"
        "<li>白名单只能追加不能删除默认集；豁免列表的黑名单同样不可绕过（见 Tab1 / Tab4）。</li>"
        "<li>用户追加的命令仍要 5s 倒计时确认；豁免列表才会跳过弹窗。</li>"
        "<li>追加危险命令前请确认它在被调试进程上下文执行不会卡死系统（参考 K-30 名单）。</li>"
        "</ul>"
        "<p style='color:#F2C84B'>已知配置文件路径：<br/><code>%1</code><br/>状态：%2</p>")
        .arg(cfgPathStr)
        .arg(cfgExists ? QStringLiteral("✅ 已存在") : QStringLiteral("⚠️ 不存在（点下方按钮可创建并打开）")));
    notes->setTextFormat(Qt::RichText);
    notes->setWordWrap(true);
    lay->addWidget(notes);

    auto* btnRow = new QHBoxLayout;
    auto* openBtn = new QPushButton(QStringLiteral("打开 config.json"));
    auto* copyBtn = new QPushButton(QStringLiteral("复制示例 JSON"));
    auto* copyPathBtn = new QPushButton(QStringLiteral("复制路径"));
    btnRow->addWidget(openBtn);
    btnRow->addWidget(copyBtn);
    btnRow->addWidget(copyPathBtn);
    btnRow->addStretch(1);
    lay->addLayout(btnRow);

    connect(openBtn, &QPushButton::clicked, this, &SafetyBrowserDialog::onOpenConfigJson);
    connect(copyBtn, &QPushButton::clicked, this, [sample]{
        QGuiApplication::clipboard()->setText(sample->toPlainText());
    });
    connect(copyPathBtn, &QPushButton::clicked, this, [cfgPathStr]{
        QGuiApplication::clipboard()->setText(cfgPathStr);
    });

    lay->addStretch(1);
    return w;
}

void SafetyBrowserDialog::onOpenConfigJson()
{
    namespace fs = std::filesystem;
    const auto cfgPath = pluginConfigFile();
    std::error_code ec;
    if (!fs::exists(cfgPath, ec)) {
        // 创建带默认骨架的空文件，方便用户直接编辑
        fs::create_directories(cfgPath.parent_path(), ec);
        std::ofstream ofs(cfgPath);
        if (ofs) {
            ofs << "{\n"
                   "  \"extra_dbg_cmd_whitelist\": [],\n"
                   "  \"auto_approve_tools\": []\n"
                   "}\n";
        }
    }
    const QString url = QStringLiteral("file:///") + fromStd(cfgPath.string()).replace('\\', '/');
    if (!QDesktopServices::openUrl(QUrl(url))) {
        QMessageBox::warning(this, QStringLiteral("打开失败"),
                             QStringLiteral("无法用默认程序打开：\n%1").arg(fromStd(cfgPath.string())));
    }
}

}  // namespace x64ai
