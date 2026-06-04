// ui/assistant_panel.cpp
#include "ui/assistant_panel.h"

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include "ai/agent_loop.h"
#include "ai/agent_preset.h"
#include "ai/agent_worker.h"
#include "ai/copilot_auth.h"
#include "ai/copilot_chat_client.h"
#include "ai/deepseek_chat_client.h"
#include "ai/embedding_client.h"
#include "ai/preset_store.h"
#include "ai/provider_manager.h"
#include "ai/tools/tool_registry.h"
#include "_plugins.h"
#include "bridgemain.h"
#include "dbg/event_bus.h"
#include "debugger/disasm_context.h"
#include "storage/project_context.h"
#include "storage/session_store.h"
#include "ui/api_key_dialog.h"
#include "ui/chat_view.h"
#include "ui/history_dialog.h"
#include "ui/login_dialog.h"
#include "ui/locator_dialog.h"
#include "ui/preset_editor_dialog.h"
#include "ui/tools_browser_dialog.h"
#include "ui/session_list.h"
#include "ui/tool_call_card.h"
#include "ui/trace_dialog.h"

#include "plugin/plugin_menus.h"
#include "util/config.h"
#include "util/logging.h"

namespace x64ai {

AssistantPanel* AssistantPanel::s_instance = nullptr;

namespace {

constexpr int kRagTopK = 4;

QString formatChunkSummary(const RetrievedChunk& rc)
{
    QString head = QStringLiteral("[%1 va=0x%2 dist=%3]")
        .arg(QString::fromStdString(rc.chunk.kind))
        .arg(rc.chunk.va, 0, 16)
        .arg(static_cast<double>(rc.distance), 0, 'f', 4);
    return head;
}

}  // namespace

AssistantPanel::AssistantPanel(QWidget* parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle(QStringLiteral("x64dbg AI 助手"));
    resize(1180, 720);

    // ---- 加载主题 QSS（仅作用于本窗口子树）----
    {
        QFile f(QStringLiteral(":/x64dbg-ai/styles/theme_dark.qss"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            setStyleSheet(QString::fromUtf8(f.readAll()));
        } else {
            XAI_LOG_WARN("theme_dark.qss not found in resources");
        }
    }

    // ---- 顶部工具条：Provider + 模型 + 刷新 + 登录 ----
    auto* topBar      = new QHBoxLayout();
    topBar->setContentsMargins(8, 6, 8, 6);
    topBar->setSpacing(6);

    auto* providerLabel = new QLabel(QStringLiteral("Provider"), this);
    providerLabel->setObjectName(QStringLiteral("sectionHeader"));

    providerBox_ = new QComboBox(this);
    providerBox_->setMinimumWidth(140);
    providerBox_->setEditable(false);
    providerBox_->setToolTip(QStringLiteral("选择 LLM 后端：Copilot（GitHub 订阅）或 DeepSeek（按量付费）"));
    // userData 存 ProviderKind 整数
    providerBox_->addItem(QStringLiteral("GitHub Copilot"),
                          static_cast<int>(ProviderKind::Copilot));
    providerBox_->addItem(QStringLiteral("DeepSeek"),
                          static_cast<int>(ProviderKind::DeepSeek));
    {
        auto k = ProviderManager::instance().currentKind();
        int idx = providerBox_->findData(static_cast<int>(k));
        if (idx >= 0) providerBox_->setCurrentIndex(idx);
    }

    auto* modelLabel  = new QLabel(QStringLiteral("模型"), this);
    modelLabel->setObjectName(QStringLiteral("sectionHeader"));

    modelBox_         = new QComboBox(this);
    modelBox_->setMinimumWidth(220);
    modelBox_->setEditable(false);
    modelBox_->setToolTip(QStringLiteral("选择模型；切换会被记录到当前会话"));

    refreshBtn_ = new QToolButton(this);
    refreshBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/refresh.svg")));
    refreshBtn_->setToolTip(QStringLiteral("从当前 Provider 拉取可用模型列表"));
    refreshBtn_->setToolButtonStyle(Qt::ToolButtonIconOnly);

    locatorBtn_ = new QToolButton(this);
    locatorBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/search.svg")));
    locatorBtn_->setText(QStringLiteral(" 扫描"));
    locatorBtn_->setToolTip(QStringLiteral("启发式定位器：API/字符串/特征码扫描"));
    locatorBtn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    traceBtn_ = new QToolButton(this);
    traceBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/git-branch.svg")));
    traceBtn_->setText(QStringLiteral(" 追溯"));
    traceBtn_->setToolTip(QStringLiteral("调用链追溯：录制 trace 并重建调用树"));
    traceBtn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    historyBtn_ = new QToolButton(this);
    historyBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/folder-clock.svg")));
    historyBtn_->setText(QStringLiteral(" 历史"));
    historyBtn_->setToolTip(QStringLiteral("浏览/导入旧版本 EXE 的会话（按 SHA256 隔离的历史项目库）"));
    historyBtn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    // ===== M4.6d Agent split button =====
    agentBtn_ = new QToolButton(this);
    agentBtn_->setText(QStringLiteral(" 工作流"));
    agentBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/play.svg")));
    agentBtn_->setToolTip(QStringLiteral("用当前激活的工作流跑一次 Agent（自主多步调工具）"));
    agentBtn_->setPopupMode(QToolButton::MenuButtonPopup);
    agentBtn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    agentMenu_ = new QMenu(this);
    agentBtn_->setMenu(agentMenu_);

    toolsBtn_ = new QToolButton(this);
    toolsBtn_->setText(QStringLiteral(" 工具列表"));
    toolsBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/tool.svg")));
    toolsBtn_->setToolTip(QStringLiteral(
        "查看已注册工具一览（含分组 / 类别 / 描述 / 当前预设启用状态）"));
    toolsBtn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolsBtn_->setPopupMode(QToolButton::DelayedPopup);
    // toolsMenu_ 保留为 nullptr；旧 popup 菜单已废弃，改为 openToolsBrowser() 弹对话框
    connect(toolsBtn_, &QToolButton::clicked, this, &AssistantPanel::openToolsBrowser);

    cancelBtn_ = new QPushButton(QStringLiteral("取消"), this);
    cancelBtn_->setToolTip(QStringLiteral("协作取消正在运行的 Agent"));
    cancelBtn_->setVisible(false);

    agentStatusLabel_ = new QLabel(this);
    agentStatusLabel_->setObjectName(QStringLiteral("agentPresetLabel"));
    agentStatusLabel_->setToolTip(QStringLiteral("当前激活工作流；底部聊天框输入回车也按此工作流跑"));

    loginBtn_ = new QToolButton(this);
    loginBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/login.svg")));
    loginBtn_->setText(QStringLiteral(" 登录"));
    loginBtn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    loginBtn_->setToolTip(QStringLiteral("登录当前 Provider"));

    loginStatus_ = new QLabel(QStringLiteral("● 未登录"), this);
    loginStatus_->setObjectName(QStringLiteral("loginStatusFail"));

    topBar->addWidget(providerLabel);
    topBar->addWidget(providerBox_);
    topBar->addSpacing(8);
    topBar->addWidget(modelLabel);
    topBar->addWidget(modelBox_, 1);
    topBar->addWidget(refreshBtn_);
    topBar->addSpacing(8);
    topBar->addWidget(agentBtn_);
    topBar->addWidget(toolsBtn_);
    topBar->addWidget(cancelBtn_);
    topBar->addWidget(agentStatusLabel_);
    topBar->addSpacing(8);
    topBar->addWidget(locatorBtn_);
    topBar->addWidget(traceBtn_);
    topBar->addWidget(historyBtn_);
    topBar->addSpacing(12);
    topBar->addWidget(loginStatus_);
    topBar->addWidget(loginBtn_);

    // ---- 左侧会话列表 + 右侧聊天 ----
    sessions_ = new SessionListWidget(this);
    sessions_->setMinimumWidth(220);
    sessions_->setMaximumWidth(360);

    chat_ = new ChatView(this);

    auto* rightWrap = new QWidget(this);
    auto* rightLay  = new QVBoxLayout(rightWrap);
    rightLay->setContentsMargins(0, 0, 0, 0);
    rightLay->setSpacing(0);
    rightLay->addLayout(topBar);
    rightLay->addWidget(chat_, 1);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(sessions_);
    split->addWidget(rightWrap);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setHandleWidth(1);

    // ---- 底部状态栏 ----
    auto* statusBar = new QFrame(this);
    statusBar->setObjectName(QStringLiteral("statusBar"));
    statusBar->setFrameShape(QFrame::NoFrame);
    auto* statusLay = new QHBoxLayout(statusBar);
    statusLay->setContentsMargins(0, 0, 0, 0);
    statusLay->setSpacing(0);
    statusProject_ = new QLabel(QStringLiteral("项目：未连接"), statusBar);
    statusModel_   = new QLabel(QStringLiteral("模型：—"),     statusBar);
    statusChunks_  = new QLabel(QStringLiteral("RAG：0"),       statusBar);
    statusLay->addWidget(statusProject_);
    statusLay->addStretch(1);
    statusLay->addWidget(statusModel_);
    statusLay->addWidget(statusChunks_);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(split, 1);
    root->addWidget(statusBar);

    chat_->appendSystemNote(QStringLiteral("已就绪。开始调试一个程序后将自动建立项目库。"));

    connect(chat_, &ChatView::userSubmitted,
            this, &AssistantPanel::onUserSubmitted);
    connect(refreshBtn_, &QToolButton::clicked,
            this, &AssistantPanel::refreshModelsAsync);
    connect(loginBtn_, &QToolButton::clicked,
            this, &AssistantPanel::onLoginClicked);
    connect(locatorBtn_, &QToolButton::clicked,
            this, &AssistantPanel::onLocatorClicked);
    connect(traceBtn_, &QToolButton::clicked,
            this, &AssistantPanel::onTraceClicked);
    connect(historyBtn_, &QToolButton::clicked,
            this, &AssistantPanel::onHistoryClicked);
    // Agent split button：默认动作 = 用当前激活预设跑（userQuery 为空）
    connect(agentBtn_, &QToolButton::clicked, this, [this]() {
        if (activePresetId_.empty()) {
            chat_->appendSystemNote(QStringLiteral("尚未选择工作流，请从下拉菜单选择。"));
            return;
        }
        runAgentWithPreset(activePresetId_, QString());
    });
    connect(cancelBtn_, &QPushButton::clicked,
            this, &AssistantPanel::onCancelAgentClicked);
    connect(providerBox_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AssistantPanel::onProviderChanged);
    connect(sessions_, &SessionListWidget::sessionSelected,
            this, [this](qint64 id) { loadSession(static_cast<int64_t>(id)); });
    connect(modelBox_, &QComboBox::currentTextChanged,
            this, [this](const QString& text) {
                if (text.isEmpty()) return;
                if (currentSessionId_ > 0) {
                    if (auto store = ProjectContext::instance().store()) {
                        bool ok = store->updateSessionModel(currentSessionId_, text.toStdString());
                        XAI_LOG_INFO("model switched to '{}' for session {} (db_ok={})",
                                     text.toUtf8().constData(), currentSessionId_, ok);
                    }
                    chat_->appendSystemNote(QStringLiteral("已切换模型为：%1").arg(text));
                } else {
                    pendingModelForNewSession_ = text;
                    XAI_LOG_INFO("model preselected '{}' (no active session yet)",
                                 text.toUtf8().constData());
                    chat_->appendSystemNote(
                        QStringLiteral("已选择模型：%1（将在创建新会话时生效）").arg(text));
                }
                updateStatusBar();
            });

    // 模型列表先留空，等 listModels 异步拉回再填，避免误用占位
    refreshLoginStatus();
    refreshModelsAsync();
    refreshSessionPanel();

    // M4.6d：装载预设并选默认（freeform 优先；否则取第一个）
    PresetStore::instance().load();
    {
        const auto& all = PresetStore::instance().presets();
        std::string defId;
        for (const auto& p : all) {
            if (p.id == "freeform") { defId = p.id; break; }
        }
        if (defId.empty() && !all.empty()) defId = all.front().id;
        setActivePreset(defId);
    }
    rebuildAgentMenu();
    // toolsMenu 已废弃，工具一览改为 openToolsBrowser() 按需弹窗

    // S3：订阅 ProjectStoreReady —— 后台 SHA256 线程装好 store 后会 publish，
    // 这里 marshal 到 GUI 线程刷新状态栏 + 会话列表。
    // handler 在 publish 线程同步执行，必须短小：只做 QMetaObject::invokeMethod。
    projectStoreReadyToken_ = EventBus::instance().subscribe(
        DbgEvent::ProjectStoreReady,
        [](const DbgEventPayload& /*p*/) {
            // 不依赖 payload.raw（已是栈拷贝），直接读 ProjectContext 单例。
            if (!s_instance) return;
            QMetaObject::invokeMethod(s_instance, [self = s_instance]() {
                self->refreshSessionPanel();   // 内部已 updateStatusBar()
                self->chat_->appendSystemNote(
                    QStringLiteral("项目已就绪：%1")
                        .arg(QString::fromStdString(
                            ProjectContext::instance().projectId().substr(0, 12))));
            }, Qt::QueuedConnection);
        });
}

AssistantPanel::~AssistantPanel()
{
    if (projectStoreReadyToken_) {
        EventBus::instance().unsubscribe(projectStoreReadyToken_);
        projectStoreReadyToken_ = 0;
    }
    if (s_instance == this) s_instance = nullptr;
}

AssistantPanel* AssistantPanel::showInstance()
{
    if (!s_instance) {
        if (QApplication::instance()) {
            s_instance = new AssistantPanel();
        } else {
            XAI_LOG_ERROR("no QApplication instance, cannot create panel");
            return nullptr;
        }
    }
    s_instance->show();
    s_instance->raise();
    s_instance->activateWindow();
    return s_instance;
}

void AssistantPanel::destroyInstance()
{
    if (s_instance) {
        s_instance->deleteLater();
        s_instance = nullptr;
    }
}

void AssistantPanel::onDebugStarted()
{
    if (!s_instance) return;
    QMetaObject::invokeMethod(s_instance, [self = s_instance]() {
        self->refreshSessionPanel();
        self->chat_->appendSystemNote(
            QStringLiteral("项目已切换：%1")
                .arg(QString::fromStdString(ProjectContext::instance().projectId().substr(0, 12))));
    }, Qt::QueuedConnection);
}

void AssistantPanel::onDebugStopped()
{
    if (!s_instance) return;
    QMetaObject::invokeMethod(s_instance, [self = s_instance]() {
        self->currentSessionId_ = 0;
        self->refreshSessionPanel();
        self->chat_->appendSystemNote(QStringLiteral("调试已停止。"));
    }, Qt::QueuedConnection);
}

void AssistantPanel::analyzeCurrentAddress()
{
    if (!s_instance) return;
    // 切到 GUI 线程后再执行（菜单回调来自 x64dbg 主线程）
    QMetaObject::invokeMethod(s_instance, []() {
        auto* self = s_instance;
        if (!self) return;

        DisasmContext ctx = captureCurrentDisasmContext(32);
        if (!ctx.debugging) {
            self->chat_->appendSystemNote(QStringLiteral("当前未在调试，无法分析。"));
            return;
        }
        if (ctx.lines.empty()) {
            self->chat_->appendSystemNote(QStringLiteral("未取到反汇编内容，请在反汇编窗口先选中地址。"));
            return;
        }

        std::string ctxMd = renderDisasmContextMarkdown(ctx);

        // 把反汇编片段写入 RAG（异步）
        if (auto store = ProjectContext::instance().store()) {
            std::string textCopy = ctxMd;
            uint64_t va = ctx.contextStart;
            QPointer<AssistantPanel> guard(self);
            QtConcurrent::run([store, textCopy, va, guard]() {
                auto emb = EmbeddingClient::instance().embed(textCopy);
                store->addChunk("asm", va, textCopy, emb);
                if (guard) {
                    QMetaObject::invokeMethod(guard.data(), [guard]() {
                        if (guard) guard->updateStatusBar();
                    }, Qt::QueuedConnection);
                }
            });
        }

        char header[96];
        std::snprintf(header, sizeof(header),
                      "AI 分析地址 0x%llX（%zu 条指令）",
                      static_cast<unsigned long long>(ctx.contextStart),
                      ctx.lines.size());

        const std::string sys =
            "You are an expert reverse engineering assistant embedded in x64dbg. "
            "Analyze the provided disassembly snippet. "
            "Identify what the function/block does, key API calls, suspicious patterns, "
            "and the likely intent. Reply concisely in Chinese with a short summary "
            "followed by a numbered breakdown of important instructions.";

        std::string user;
        user.reserve(ctxMd.size() + 128);
        user.append("请分析以下 x64dbg 反汇编片段：\n\n");
        user.append(ctxMd);

        self->sendChat(sys, user, QString::fromUtf8(header), /*saveToStore=*/true);
    }, Qt::QueuedConnection);
}

void AssistantPanel::submitExternalPrompt(const QString& userVisible,
                                          const std::string& userPrompt)
{
    auto* self = showInstance();
    if (!self) return;
    QMetaObject::invokeMethod(self, [self, userVisible, userPrompt]() {
        const std::string sys =
            "You are an expert reverse engineering assistant embedded in x64dbg. "
            "Answer concisely in Chinese unless the user explicitly requests otherwise.";
        self->sendChat(sys, userPrompt, userVisible, /*saveToStore=*/true);
    }, Qt::QueuedConnection);
}

void AssistantPanel::populateModels(const QStringList& ids, const QString& preferred)
{
    modelBox_->blockSignals(true);
    modelBox_->clear();
    modelBox_->addItems(ids);

    int idx = ids.indexOf(preferred);
    if (idx < 0) idx = 0;
    if (idx >= 0 && idx < ids.size()) modelBox_->setCurrentIndex(idx);

    modelBox_->blockSignals(false);
}

void AssistantPanel::refreshModelsAsync()
{
    refreshBtn_->setEnabled(false);
    chat_->appendSystemNote(QStringLiteral("正在拉取可用模型..."));

    auto* self = this;
    auto kind = ProviderManager::instance().currentKind();
    QtConcurrent::run([self, kind]() {
        auto* prov = ProviderManager::get(kind);
        auto vec = prov->listModels();
        QStringList ids;
        ids.reserve(static_cast<int>(vec.size()));
        for (const auto& s : vec) ids << QString::fromStdString(s);

        const QString preferred = QString::fromStdString(prov->defaultModel());

        QMetaObject::invokeMethod(self, [self, ids, preferred, kind]() {
            // 防止用户在拉取期间已切换 Provider，丢弃过期结果
            if (ProviderManager::instance().currentKind() != kind) {
                self->refreshBtn_->setEnabled(true);
                return;
            }
            self->refreshBtn_->setEnabled(true);
            if (ids.isEmpty()) {
                self->chat_->appendSystemNote(
                    QStringLiteral("拉取模型失败或为空，使用默认：%1").arg(preferred));
                self->populateModels(QStringList{} << preferred, preferred);
            } else {
                self->chat_->appendSystemNote(
                    QStringLiteral("拉取到 %1 个模型").arg(ids.size()));
                self->populateModels(ids, preferred);
            }
        }, Qt::QueuedConnection);
    });
}

void AssistantPanel::refreshSessionPanel()
{
    sessions_->refresh();
    updateStatusBar();
}

void AssistantPanel::updateStatusBar()
{
    if (!statusProject_) return;

    auto store = ProjectContext::instance().store();
    if (store) {
        const auto pid = ProjectContext::instance().projectId();
        statusProject_->setText(QStringLiteral("项目  %1…")
                                    .arg(QString::fromStdString(pid.substr(0, 12))));
        statusChunks_->setText(QStringLiteral("RAG  %1").arg(store->chunkCount()));
    } else {
        statusProject_->setText(QStringLiteral("项目  未连接"));
        statusChunks_->setText(QStringLiteral("RAG  0"));
    }

    QString m = modelBox_ ? modelBox_->currentText().trimmed() : QString();
    statusModel_->setText(QStringLiteral("模型  %1").arg(m.isEmpty() ? QStringLiteral("—") : m));
}

void AssistantPanel::refreshLoginStatus()
{
    auto kind = ProviderManager::instance().currentKind();
    if (kind == ProviderKind::Copilot) {
        auto user = CopilotAuth::instance().currentUserLogin();
        if (user && !user->empty()) {
            loginStatus_->setText(QStringLiteral("● Copilot：%1")
                                      .arg(QString::fromStdString(*user)));
            loginStatus_->setObjectName(QStringLiteral("loginStatusOk"));
            loginBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/logout.svg")));
            loginBtn_->setText(QStringLiteral(" 登出"));
            loginBtn_->setToolTip(QStringLiteral("清除本插件保存的 GitHub 登录凭据"));
        } else {
            loginStatus_->setText(QStringLiteral("● Copilot 未登录"));
            loginStatus_->setObjectName(QStringLiteral("loginStatusFail"));
            loginBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/login.svg")));
            loginBtn_->setText(QStringLiteral(" 登录"));
            loginBtn_->setToolTip(QStringLiteral("使用 GitHub Device Flow 登录 Copilot"));
        }
    } else {
        // DeepSeek
        auto masked = DeepSeekChatClient::instance().maskedApiKey();
        if (!masked.empty()) {
            loginStatus_->setText(QStringLiteral("● DeepSeek：%1")
                                      .arg(QString::fromStdString(masked)));
            loginStatus_->setObjectName(QStringLiteral("loginStatusOk"));
            loginBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/logout.svg")));
            loginBtn_->setText(QStringLiteral(" 修改 Key"));
            loginBtn_->setToolTip(QStringLiteral("修改或清除已保存的 DeepSeek API Key"));
        } else {
            loginStatus_->setText(QStringLiteral("● DeepSeek 未设置 Key"));
            loginStatus_->setObjectName(QStringLiteral("loginStatusFail"));
            loginBtn_->setIcon(QIcon(QStringLiteral(":/x64dbg-ai/icons/login.svg")));
            loginBtn_->setText(QStringLiteral(" 设置 Key"));
            loginBtn_->setToolTip(QStringLiteral("输入 DeepSeek API Key（DPAPI 加密保存）"));
        }
    }
    loginStatus_->style()->unpolish(loginStatus_);
    loginStatus_->style()->polish(loginStatus_);
}

void AssistantPanel::onLoginClicked()
{
    auto kind = ProviderManager::instance().currentKind();
    if (kind == ProviderKind::Copilot) {
        auto user = CopilotAuth::instance().currentUserLogin();
        if (user && !user->empty()) {
            auto ret = QMessageBox::question(
                this, QStringLiteral("确认登出"),
                QStringLiteral("确定要登出 GitHub Copilot 吗？\n（仅清除本插件写入的 token，不影响 gh / VSCode）"),
                QMessageBox::Yes | QMessageBox::No);
            if (ret != QMessageBox::Yes) return;
            CopilotAuth::instance().logout();
            chat_->appendSystemNote(QStringLiteral("已登出 Copilot。"));
            refreshLoginStatus();
            return;
        }
        LoginDialog dlg(this);
        bool ok = dlg.runLogin();
        if (ok) {
            chat_->appendSystemNote(QStringLiteral("Copilot 登录成功。"));
            refreshLoginStatus();
            refreshModelsAsync();
        } else {
            chat_->appendSystemNote(QStringLiteral("Copilot 登录未完成。"));
        }
        return;
    }

    // DeepSeek 分支
    ApiKeyDialog dlg(this);
    int ret = dlg.exec();
    if (ret == QDialog::Accepted) {
        QString k = dlg.apiKey();
        if (!k.isEmpty()) {
            bool ok = DeepSeekChatClient::instance().saveApiKey(k.toStdString());
            chat_->appendSystemNote(ok
                ? QStringLiteral("DeepSeek API Key 已保存。")
                : QStringLiteral("DeepSeek API Key 保存失败。"));
        } else if (dlg.cleared()) {
            chat_->appendSystemNote(QStringLiteral("已清除 DeepSeek API Key。"));
        }
        refreshLoginStatus();
        refreshModelsAsync();
    }
}

void AssistantPanel::onProviderChanged(int /*index*/)
{
    int dataVal = providerBox_->currentData().toInt();
    auto kind = static_cast<ProviderKind>(dataVal);
    if (ProviderManager::instance().currentKind() == kind) return;

    ProviderManager::instance().setProvider(kind);
    chat_->appendSystemNote(QStringLiteral("已切换 Provider：%1")
                                .arg(providerBox_->currentText()));

    // 清空模型下拉，避免误用旧 Provider 的模型
    modelBox_->blockSignals(true);
    modelBox_->clear();
    modelBox_->blockSignals(false);
    pendingModelForNewSession_.clear();

    refreshLoginStatus();
    refreshModelsAsync();
    updateStatusBar();
}

void AssistantPanel::loadSession(int64_t sessionId)
{
    auto store = ProjectContext::instance().store();
    if (!store || sessionId <= 0) return;

    currentSessionId_ = sessionId;
    chat_->clearTranscript();
    chat_->appendSystemNote(QStringLiteral("已切换到会话 #%1").arg(sessionId));

    // 恢复该会话上次使用的模型（若下拉列表里存在）
    auto allSes = store->listSessions();
    for (const auto& s : allSes) {
        if (s.id == sessionId && !s.model.empty()) {
            QString m = QString::fromStdString(s.model);
            int idx = modelBox_->findText(m);
            if (idx >= 0) {
                modelBox_->blockSignals(true);
                modelBox_->setCurrentIndex(idx);
                modelBox_->blockSignals(false);
            }
            break;
        }
    }

    auto msgs = store->listMessages(sessionId);
    for (const auto& m : msgs) {
        QString c = QString::fromStdString(m.content);
        if (m.role == "user") {
            chat_->appendUserMessage(c);
        } else if (m.role == "assistant") {
            chat_->appendAssistantHeader();
            chat_->appendAssistantDelta(c);
            chat_->finalizeAssistantMessage();
        } else {
            chat_->appendSystemNote(QStringLiteral("[%1] %2")
                                       .arg(QString::fromStdString(m.role)).arg(c));
        }
    }
    updateStatusBar();
}

int64_t AssistantPanel::ensureSession()
{
    auto store = ProjectContext::instance().store();
    if (!store) return 0;
    if (currentSessionId_ > 0) {
        // 校验是否仍存在
        auto all = store->listSessions();
        for (const auto& s : all) {
            if (s.id == currentSessionId_) return currentSessionId_;
        }
    }
    // 没有则建一个默认会话
    int64_t id = store->createSession("默认会话",
                                      modelBox_->currentText().toStdString());
    if (id > 0) {
        currentSessionId_ = id;
        sessions_->refresh();
    }
    return id;
}

std::string AssistantPanel::buildRagAugmentedPrompt(const std::string& userPrompt)
{
    auto store = ProjectContext::instance().store();
    if (!store) return userPrompt;

    auto qEmb = EmbeddingClient::instance().embed(userPrompt);
    if (qEmb.empty()) return userPrompt;

    auto hits = store->searchSimilar(qEmb, kRagTopK);
    if (hits.empty()) return userPrompt;

    std::string out;
    out.reserve(userPrompt.size() + 4096);
    out.append("# Project context (top-");
    out.append(std::to_string(hits.size()));
    out.append(" related snippets retrieved by semantic search)\n\n");
    int i = 1;
    for (const auto& h : hits) {
        char head[160];
        std::snprintf(head, sizeof(head),
                      "## [%d] kind=%s va=0x%llX distance=%.4f\n",
                      i++, h.chunk.kind.c_str(),
                      static_cast<unsigned long long>(h.chunk.va),
                      static_cast<double>(h.distance));
        out.append(head);
        out.append(h.chunk.text);
        out.append("\n\n");
    }
    out.append("# User question\n");
    out.append(userPrompt);
    return out;
}

void AssistantPanel::sendChat(const std::string& systemPrompt,
                              const std::string& userPrompt,
                              const QString&     userVisibleText,
                              bool               saveToStore)
{
    QString reqModelQ = modelBox_->currentText().trimmed();
    if (reqModelQ.isEmpty()) {
        chat_->appendSystemNote(QStringLiteral(
            "⚠ 当前未选择模型，本次请求已取消。请先登录 GitHub Copilot 并在右上角下拉框中选择一个模型。"));
        QMessageBox::information(this,
            QStringLiteral("请先选择模型"),
            QStringLiteral("当前未选择任何模型。\n\n请先点击右上角【登录】完成 GitHub Copilot 登录，"
                           "等待模型列表加载完成后，从下拉框中选择一个模型再继续。"));
        return;
    }

    if (!userVisibleText.isEmpty()) {
        chat_->appendUserMessage(userVisibleText);
    }
    chat_->appendAssistantHeader();

    int64_t sessionId = saveToStore ? ensureSession() : 0;
    auto    store     = ProjectContext::instance().store();

    // RAG 拼接（若有 store + PAT 可用；失败则原样使用）
    std::string augmentedUser = userPrompt;
    if (store) {
        // 注意：会发出一次 embedding HTTP，应放后台 / 接受同步阻塞
        // 这里仍在 GUI 线程；为避免卡顿，转后台执行后再发起 chat
    }

    auto* view = chat_;
    auto  reqModel = reqModelQ.toStdString();

    // 把消息体（含 RAG 阶段）整体放后台
    QtConcurrent::run([this, store, sessionId, reqModel,
                       systemPrompt, userPrompt, view]() {
        std::string finalUser = userPrompt;
        if (store) {
            finalUser = const_cast<AssistantPanel*>(this)->buildRagAugmentedPrompt(userPrompt);
        }

        // 装载会话历史（必须在 appendMessage 之前，避免把当前轮算进去）
        std::vector<MessageRow> history;
        if (store && sessionId > 0) {
            history = store->listMessages(sessionId);
        }

        // 持久化用户消息（保存原始 prompt，不存 RAG 增强后的）
        if (store && sessionId > 0) {
            store->appendMessage(sessionId, "user", userPrompt);
        }

        ChatRequest req;
        req.model = reqModel;
        req.messages.push_back({"system", systemPrompt});
        // 历史消息按时序回放（只取 user/assistant，跳过 system/tool）
        for (const auto& m : history) {
            if (m.role == "user" || m.role == "assistant") {
                req.messages.push_back({m.role, m.content});
            }
        }
        req.messages.push_back({"user",   finalUser});

        // 累积 assistant 输出以便落库
        auto accumulated = std::make_shared<std::string>();

        ChatStreamCallbacks cb;
        cb.onDelta = [view, accumulated](std::string_view delta) {
            accumulated->append(delta);
            QString s = QString::fromUtf8(delta.data(), static_cast<int>(delta.size()));
            QMetaObject::invokeMethod(view, [view, s]() {
                view->appendAssistantDelta(s);
            }, Qt::QueuedConnection);
        };
        cb.onError = [view](std::string err) {
            QString msg = QString::fromStdString(err);
            QMetaObject::invokeMethod(view, [view, msg]() {
                view->appendSystemNote(QStringLiteral("错误：") + msg);
            }, Qt::QueuedConnection);
        };
        cb.onDone = [view, store, sessionId, accumulated]() {
            if (store && sessionId > 0 && !accumulated->empty()) {
                store->appendMessage(sessionId, "assistant", *accumulated);
            }
            QMetaObject::invokeMethod(view, [view]() {
                view->finalizeAssistantMessage();
            }, Qt::QueuedConnection);
        };

        ProviderManager::instance().current()->streamChat(req, cb);
    });
}

void AssistantPanel::onUserSubmitted(const QString& text)
{
    XAI_LOG_INFO("user submitted: {}", text.toUtf8().constData());

    // M4.6d：始终走当前激活预设。
    // freeform 预设的 userTemplate = "{{user}}"，行为等价于旧的纯聊天。
    if (!activePresetId_.empty()) {
        runAgentWithPreset(activePresetId_, text);
        return;
    }

    // 兜底：未设置任何预设（理论上构造时已强制选了一个，这里防御性写）
    sendChat(
        "You are an expert reverse engineering assistant embedded in x64dbg. "
        "Answer concisely in Chinese unless the user explicitly requests otherwise.",
        text.toStdString(),
        QString(),
        /*saveToStore=*/true);
}

void AssistantPanel::onLocatorClicked()
{
    if (!DbgIsDebugging()) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("请先开始调试一个程序，再使用启发式定位器。"));
        return;
    }
    auto* dlg = new LocatorDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    // 应用同款主题（QSS 仅作用于本窗口子树，所以子对话框需要显式继承）
    QFile f(QStringLiteral(":/x64dbg-ai/styles/theme_dark.qss"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        dlg->setStyleSheet(QString::fromUtf8(f.readAll()));
    }
    connect(dlg, &QDialog::finished, this, [this](int) {
        // 扫描可能写了 RAG，更新状态栏 chunk 数
        updateStatusBar();
    });
    dlg->show();
}

void AssistantPanel::onTraceClicked()
{
    auto* dlg = new TraceDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    QFile f(QStringLiteral(":/x64dbg-ai/styles/theme_dark.qss"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        dlg->setStyleSheet(QString::fromUtf8(f.readAll()));
    }
    dlg->show();
}

void AssistantPanel::onHistoryClicked()
{
    auto* dlg = new HistoryDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    // HistoryDialog 构造时会复制父 styleSheet；保险再 polish 一次（QSS 已设过）
    connect(dlg, &HistoryDialog::imported,
            this, &AssistantPanel::refreshSessionPanel);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void AssistantPanel::traceFunctionAt(uint64_t va)
{
    // 菜单回调可能不在 GUI 线程：把整个流程 marshal 到主线程
    QMetaObject::invokeMethod(qApp, [va]() {
        auto* panel = showInstance();
        if (!panel) return;

        auto* dlg = new TraceDialog(panel);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        QFile f(QStringLiteral(":/x64dbg-ai/styles/theme_dark.qss"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            dlg->setStyleSheet(QString::fromUtf8(f.readAll()));
        }
        dlg->openForTarget(va);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    }, Qt::QueuedConnection);
}

// ============================================================
// M4.6d Agent 模式实现
// ============================================================

void AssistantPanel::setActivePreset(const std::string& presetId)
{
    activePresetId_ = presetId;
    if (!agentStatusLabel_) return;

    auto opt = PresetStore::instance().findById(presetId);
    if (opt) {
        // G-2: 末尾拼最近一轮 cache 状态（若有）
        QString text = QStringLiteral("[%1]").arg(QString::fromStdString(opt->name));
        if (!lastCacheStatus_.isEmpty()) {
            text += QStringLiteral(" · ") + lastCacheStatus_;
        }
        agentStatusLabel_->setText(text);
        agentBtn_->setToolTip(QStringLiteral("用工作流「%1」跑 Agent：%2")
                                  .arg(QString::fromStdString(opt->name),
                                       QString::fromStdString(opt->description)));
    } else {
        agentStatusLabel_->setText(QStringLiteral("[未选择]"));
    }
}

void AssistantPanel::rebuildAgentMenu()
{
    if (!agentMenu_) return;
    agentMenu_->clear();

    const auto& presets = PresetStore::instance().presets();
    for (const auto& p : presets) {
        QString label = QString::fromStdString(p.name);
        if (p.readonly) label += QStringLiteral("  (内置)");
        QAction* act = agentMenu_->addAction(label);
        act->setToolTip(QString::fromStdString(p.description));
        std::string pid = p.id;
        connect(act, &QAction::triggered, this, [this, pid]() {
            setActivePreset(pid);
            // 选中即视为想立刻跑一次（userQuery 留空，用模板占位符）
            runAgentWithPreset(pid, QString());
        });
    }

    agentMenu_->addSeparator();
    QAction* mgr = agentMenu_->addAction(QStringLiteral("管理工作流…"));
    connect(mgr, &QAction::triggered, this, &AssistantPanel::openPresetManager);
}

void AssistantPanel::rebuildToolsMenu()
{
    // 旧的工具 popup-menu 已废弃（S9++ 改为 openToolsBrowser() 弹只读对话框）
    // 保留方法签名以兼容外部潜在调用；内部不做任何事
}

void AssistantPanel::openToolsBrowser()
{
    ToolsBrowserDialog dlg(this, activePresetId_);
    QFile f(QStringLiteral(":/x64dbg-ai/styles/theme_dark.qss"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        dlg.setStyleSheet(QString::fromUtf8(f.readAll()));
    }
    dlg.exec();
    if (dlg.openPresetEditorRequested()) {
        openPresetManager();
    }
}

void AssistantPanel::openPresetManager()
{
    PresetEditorDialog dlg(this);
    QFile f(QStringLiteral(":/x64dbg-ai/styles/theme_dark.qss"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        dlg.setStyleSheet(QString::fromUtf8(f.readAll()));
    }
    dlg.exec();
    if (dlg.changed()) {
        PresetStore::instance().reload();
        rebuildAgentMenu();
        rebuildDisasmAiSubmenu();
        // 若当前 activePreset 已被删除，回退到第一个
        if (!PresetStore::instance().findById(activePresetId_)) {
            const auto& all = PresetStore::instance().presets();
            setActivePreset(all.empty() ? std::string() : all.front().id);
        }
    }
}

void AssistantPanel::setAgentRunning(bool running)
{
    if (cancelBtn_)  cancelBtn_->setVisible(running);
    if (agentBtn_)   agentBtn_->setEnabled(!running);
    if (toolsBtn_)   toolsBtn_->setEnabled(!running);
    if (providerBox_) providerBox_->setEnabled(!running);
    if (modelBox_)    modelBox_->setEnabled(!running);
}

void AssistantPanel::onCancelAgentClicked()
{
    if (agentCancel_) {
        agentCancel_->store(true);
        XAI_LOG_INFO("agent cancel requested by user");
        chat_->appendSystemNote(QStringLiteral("已请求取消 Agent…"));
    }
    if (agentWorker_) {
        agentWorker_->requestCancel();
    }
}

void AssistantPanel::runAgentWithPreset(const std::string& presetId,
                                        const QString&     userQuery)
{
    if (agentWorker_ && agentWorker_->isRunning()) {
        chat_->appendSystemNote(QStringLiteral("Agent 正在运行，请先等待或点击取消。"));
        return;
    }

    auto presetOpt = PresetStore::instance().findById(presetId);
    if (!presetOpt) {
        chat_->appendSystemNote(QStringLiteral("未找到预设：%1")
                                    .arg(QString::fromStdString(presetId)));
        return;
    }
    const AgentPreset preset = *presetOpt;

    // ---- 1. provider 选择 ----
    ProviderKind kind = ProviderManager::instance().currentKind();
    if (preset.provider == "deepseek") kind = ProviderKind::DeepSeek;
    else if (preset.provider == "copilot") kind = ProviderKind::Copilot;
    auto* provider = ProviderManager::get(kind);
    if (!provider) {
        chat_->appendSystemNote(QStringLiteral("无法获取 Provider。"));
        return;
    }

    // ---- 2. 模型 ----
    std::string model = !preset.model.empty()
                            ? preset.model
                            : modelBox_->currentText().trimmed().toStdString();
    if (model.empty()) model = provider->defaultModel();
    if (model.empty()) {
        chat_->appendSystemNote(QStringLiteral("⚠ 未选择模型，请先登录并选择一个模型。"));
        return;
    }

    // ---- 3. 抓 disasm 上下文，渲染模板 ----
    PresetTemplateContext tctx;
    DisasmContext dctx = captureCurrentDisasmContext(32);
    if (dctx.debugging && dctx.contextStart != 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%016llX",
                      static_cast<unsigned long long>(dctx.contextStart));
        tctx.cipHex = buf;
        char modBuf[MAX_MODULE_SIZE] = {0};
        if (DbgGetModuleAt(static_cast<duint>(dctx.contextStart), modBuf)) {
            tctx.moduleName = modBuf;
        }
        if (dctx.selectionStart != 0 || dctx.selectionEnd != 0) {
            char sbuf[64];
            std::snprintf(sbuf, sizeof(sbuf), "0x%llX - 0x%llX",
                          static_cast<unsigned long long>(dctx.selectionStart),
                          static_cast<unsigned long long>(dctx.selectionEnd));
            tctx.selectionRange = sbuf;
        } else {
            tctx.selectionRange = "(none)";
        }
        tctx.disasmBlock = renderDisasmContextMarkdown(dctx);
    } else {
        tctx.cipHex = "(not debugging)";
        tctx.moduleName = "(none)";
        tctx.selectionRange = "(none)";
        tctx.disasmBlock = "(no disassembly: debugger inactive)";
    }
    tctx.userQuery = userQuery.toStdString();

    std::string renderedUser = expandPresetTemplate(preset.userTemplate, tctx);
    if (renderedUser.empty()) {
        // freeform 模板就是 {{user}}；若用户没输入就拒绝
        chat_->appendSystemNote(QStringLiteral("请在底部输入框输入问题。"));
        return;
    }

    // ---- 4. UI 显示 user 消息 ----
    QString visibleUser = userQuery.isEmpty()
                             ? QString::fromStdString("[" + preset.name + "]")
                             : userQuery;
    chat_->appendUserMessage(visibleUser);
    chat_->appendAssistantHeader();

    // ---- 5. 会话 / 持久化 ----
    int64_t sessionId = ensureSession();
    auto    store     = ProjectContext::instance().store();
    if (store && sessionId > 0) {
        // 持久化的是模板渲染后的 user 内容（便于复现）
        store->appendMessage(sessionId, "user", renderedUser);
    }

    // ---- 6. 构造 ChatRequest ----
    ChatRequest req;
    req.model       = model;
    req.temperature = preset.temperature;
    req.stream      = true;

    if (!preset.systemPrompt.empty()) {
        ChatMessage sys; sys.role = "system"; sys.content = preset.systemPrompt;
        req.messages.push_back(std::move(sys));
    }
    // 装载历史 user/assistant
    if (store && sessionId > 0) {
        auto history = store->listMessages(sessionId);
        for (const auto& m : history) {
            // 跳过刚刚 append 的当前轮
            if (m.role == "user" || m.role == "assistant") {
                ChatMessage cm; cm.role = m.role; cm.content = m.content;
                req.messages.push_back(std::move(cm));
            }
        }
    } else {
        ChatMessage um; um.role = "user"; um.content = renderedUser;
        req.messages.push_back(std::move(um));
    }

    // ---- 7. tools 列表（按 preset.enabledTools 过滤）----
    auto allTools = ToolRegistry::instance().listChatTools();
    if (preset.enabledTools.empty()) {
        req.tools = allTools;
    } else {
        for (const auto& t : allTools) {
            for (const auto& name : preset.enabledTools) {
                if (t.name == name) { req.tools.push_back(t); break; }
            }
        }
    }
    if (!req.tools.empty()) req.toolChoice = "auto";

    // ---- 8. 起 worker ----
    AgentRunRequest arr;
    arr.provider    = provider;
    arr.model       = model;
    arr.messages    = std::move(req.messages);
    arr.temperature = preset.temperature;
    arr.maxIter     = preset.maxIter > 0 ? preset.maxIter : 20;
    arr.tools       = std::move(req.tools);   // preset.enabledTools 过滤后的白名单

    auto* worker = new AgentWorker(this);
    agentWorker_ = worker;
    agentTerminalEventHandled_ = false;  // K-13: 新一次 run，重置去重标志
    setAgentRunning(true);
    wireAgentWorker(worker, sessionId, preset.id);
    worker->start(std::move(arr));
}

// K-41d: 续跑入口。最小副作用——仅 1) lookup provider/tools 2) 起新 worker。
// 不动 UI（不 appendUserMessage / 不 appendInlineButton），只 appendAssistantHeader
// 让随后的 assistantDelta 有一个新的 assistant 框落字。
void AssistantPanel::continueAgentFromSnapshot(const std::string&       presetId,
                                               int64_t                  sessionId,
                                               std::vector<ChatMessage> snapshot,
                                               int                      extraIter)
{
    if (agentWorker_ && agentWorker_->isRunning()) {
        chat_->appendSystemNote(QStringLiteral("Agent 正在运行，请先等待或点击取消。"));
        return;
    }
    if (snapshot.empty()) {
        chat_->appendSystemNote(QStringLiteral("续跑失败：消息快照为空。"));
        return;
    }
    auto presetOpt = PresetStore::instance().findById(presetId);
    if (!presetOpt) {
        chat_->appendSystemNote(QStringLiteral("续跑失败：未找到预设 %1。")
                                    .arg(QString::fromStdString(presetId)));
        return;
    }
    const AgentPreset preset = *presetOpt;

    ProviderKind kind = ProviderManager::instance().currentKind();
    if (preset.provider == "deepseek") kind = ProviderKind::DeepSeek;
    else if (preset.provider == "copilot") kind = ProviderKind::Copilot;
    auto* provider = ProviderManager::get(kind);
    if (!provider) {
        chat_->appendSystemNote(QStringLiteral("续跑失败：无法获取 Provider。"));
        return;
    }

    std::string model = !preset.model.empty()
                            ? preset.model
                            : modelBox_->currentText().trimmed().toStdString();
    if (model.empty()) model = provider->defaultModel();
    if (model.empty()) {
        chat_->appendSystemNote(QStringLiteral("续跑失败：未选择模型。"));
        return;
    }

    // tools 白名单按当前 preset.enabledTools 重过滤（用户可能调整过）
    auto allTools = ToolRegistry::instance().listChatTools();
    std::vector<ChatTool> tools;
    if (preset.enabledTools.empty()) {
        tools = allTools;
    } else {
        for (const auto& t : allTools) {
            for (const auto& name : preset.enabledTools) {
                if (t.name == name) { tools.push_back(t); break; }
            }
        }
    }

    AgentRunRequest arr;
    arr.provider    = provider;
    arr.model       = model;
    arr.messages    = std::move(snapshot);   // K-41 核心：完整带 tool_calls 配对的快照
    arr.temperature = preset.temperature;
    arr.maxIter     = extraIter > 0 ? extraIter : 10;
    arr.tools       = std::move(tools);

    // UI：仅给 assistant 一个新框，不显示新 user 卡片
    chat_->appendSystemNote(QStringLiteral("继续推理（+%1 轮）…").arg(arr.maxIter));
    chat_->appendAssistantHeader();

    auto* worker = new AgentWorker(this);
    agentWorker_ = worker;
    agentTerminalEventHandled_ = false;
    setAgentRunning(true);
    // 复用同 sessionId + presetId，tool/assistant 消息继续 append 到同会话
    wireAgentWorker(worker, sessionId, presetId);
    worker->start(std::move(arr));
}

void AssistantPanel::wireAgentWorker(AgentWorker* w,
                                     int64_t sessionId,
                                     const std::string& presetId)
{
    auto* view = chat_;
    auto  store = ProjectContext::instance().store();

    // 累积本轮 assistant 文本，用于落库
    auto accumulated = std::make_shared<std::string>();

    connect(w, &AgentWorker::assistantDelta, this, [view, accumulated](QString d) {
        accumulated->append(d.toUtf8().constData());
        view->appendAssistantDelta(d);
    });

    connect(w, &AgentWorker::assistantReasoningDelta, this, [view](QString d) {
        view->appendAssistantReasoningDelta(d);
    });

    connect(w, &AgentWorker::assistantMessage, this,
            [view, store, sessionId, accumulated](QString content,
                                                  QStringList ids,
                                                  QStringList names,
                                                  QString toolCallsJson) {
        // 一轮 assistant 收尾：固化气泡
        view->finalizeAssistantMessage();
        if (store && sessionId > 0 &&
            (!accumulated->empty() || !toolCallsJson.isEmpty())) {
            // K-41c: 落盘 assistant 消息 + 本轮 tool_calls JSON。
            // 关键：模型可能"空 content + 仅调工具"，老条件 `!accumulated->empty()` 会
            // 漏掉这种消息（tool_calls 跟着丢），后续 tool 消息就成了"孤儿"——续跑时
            // provider 报 400（assistant.tool_calls 缺失而 role=tool 已存在）。
            // toolCallsJson 形如 `[{"id":"call_x","type":"function","function":...}]`，
            // 无工具调用时为空串。toolCallId / toolName 仅 role=tool 时填，这里留空。
            store->appendMessageEx(sessionId, "assistant", *accumulated,
                                   /*toolCallId*/ "",
                                   /*toolName*/   "",
                                   /*toolCalls*/  toolCallsJson.toStdString());
        }
        accumulated->clear();

        // 工具卡片预创建（pending）
        const int n = std::min(ids.size(), names.size());
        for (int i = 0; i < n; ++i) {
            ToolCallCard* card = view->addToolCallCard(ids[i], names[i]);
            if (card) card->setPending();
        }
    });

    connect(w, &AgentWorker::toolCallStarted, this,
            [view](QString id, QString name, QString argsJson) {
        ToolCallCard* card = view->findToolCallCard(id);
        if (!card) card = view->addToolCallCard(id, name);
        if (card) card->setRunning(argsJson);
    });

    connect(w, &AgentWorker::toolCallFinished, this,
            [view, store, sessionId](QString id, QString name, QString argsJson,
                                     QString resultJson, bool ok, QString error,
                                     qint64 elapsedMs, bool truncated) {
        ToolCallCard* card = view->findToolCallCard(id);
        if (!card) card = view->addToolCallCard(id, name);
        if (card) {
            if (ok) card->setDone(argsJson, resultJson, elapsedMs, truncated);
            else    card->setError(argsJson, error, elapsedMs);
        }

        // K-41c: 持久化 tool 消息，带 tool_call_id + tool_name（OpenAI 协议强约束：
        // role=tool 必须配对 assistant.tool_calls[i].id，否则 provider 报 400）。
        // 注意 content 现在直接用 resultJson 裸串（原有 `[tool:NAME] ...` 前缀是给老
        // 重放 UI 看的人类可读格式；续跑时 LLM 自己看 tool_name + tool_call_id 已够，
        // 不需要前缀；老 history_dialog 重放走 ProjectBrowser 路径仍可看 content 直读）。
        if (store && sessionId > 0) {
            store->appendMessageEx(sessionId, "tool", resultJson.toStdString(),
                                   /*toolCallId*/ id.toStdString(),
                                   /*toolName*/   name.toStdString(),
                                   /*toolCalls*/  "");
        }
    });

    connect(w, &AgentWorker::failed, this, [this, view](QString err) {
        view->appendSystemNote(QStringLiteral("Agent 错误：") + err);
        agentTerminalEventHandled_ = true;  // K-13
        setAgentRunning(false);
    });

    connect(w, &AgentWorker::maxIterReached, this,
            [this, view, presetId, sessionId, w](int iter, int pending) {
        view->appendSystemNote(
            QStringLiteral("Agent 达到最大迭代 %1，仍有 %2 个待执行工具。")
                .arg(iter).arg(pending));
        // K-41d: 把 worker 当下的完整 messages snapshot 取走（含 tool/tool_calls 配对）。
        // 必须在主线程槽内同步取——此时 worker 后台线程已先填好 snapshot 再 emit signal
        // (queued connection 的 happens-before 保证可见性)，且 worker 对象尚未 deleteLater。
        // 取走后用 shared_ptr 包，按钮 lambda 可多次"理论上"重用；实际只点一次。
        auto snap = std::make_shared<std::vector<ChatMessage>>(
            w ? w->takeSnapshotMessages() : std::vector<ChatMessage>{});
        view->appendInlineButton(QStringLiteral("继续推理 +10 轮"),
            [this, presetId, sessionId, snap]() {
                if (!snap || snap->empty()) {
                    chat_->appendSystemNote(
                        QStringLiteral("续跑失败：上一轮快照为空（可能已被消耗）。"));
                    return;
                }
                continueAgentFromSnapshot(presetId, sessionId, *snap, 10);
            });
        agentTerminalEventHandled_ = true;  // K-13
        setAgentRunning(false);
    });

    // G-2 (2026-05-25): cache 命中观测——更新 agentStatusLabel_ 末尾的 cache 状态
    connect(w, &AgentWorker::usageUpdated, this,
            [this](int promptTokens, int cachedPromptTokens,
                   int /*completionTokens*/, int /*reasoningTokens*/,
                   double hitRatio) {
        if (hitRatio < 0.0) {
            lastCacheStatus_ = QStringLiteral("input=%1 cache=n/a").arg(promptTokens);
        } else {
            lastCacheStatus_ = QStringLiteral("input=%1 cache=%2%")
                                   .arg(promptTokens)
                                   .arg(QString::number(hitRatio * 100.0, 'f', 1));
        }
        if (agentStatusLabel_) {
            agentStatusLabel_->setToolTip(
                QStringLiteral("最近一轮 LLM 调用：input=%1 / cached=%2 / hit_ratio=%3")
                    .arg(promptTokens).arg(cachedPromptTokens)
                    .arg(hitRatio < 0.0 ? QStringLiteral("n/a")
                                        : QString::number(hitRatio * 100.0, 'f', 1) + QStringLiteral("%")));
        }
        // 重画 label（带最新 cache 状态）
        setActivePreset(activePresetId_);
    });

    connect(w, &AgentWorker::finished, this, [this, view](int iter) {
        // K-13: failed / maxIterReached 已经处理过终态时，跳过冗余 "agent finished" 日志
        if (agentTerminalEventHandled_) {
            setAgentRunning(false);  // 仍保险地置一次（幂等）
            return;
        }
        XAI_LOG_INFO("agent finished after {} iterations", iter);
        setAgentRunning(false);
    });

    connect(w, &AgentWorker::finished, w, &QObject::deleteLater);
    connect(w, &AgentWorker::failed,   w, &QObject::deleteLater);
    connect(w, &AgentWorker::maxIterReached, w, &QObject::deleteLater);
}

void AssistantPanel::runPresetById(const std::string& presetId,
                                   const QString&     userQuery)
{
    auto* panel = showInstance();
    if (!panel) return;
    QMetaObject::invokeMethod(panel, [panel, presetId, userQuery]() {
        if (PresetStore::instance().findById(presetId)) {
            panel->runAgentWithPreset(presetId, userQuery);
        } else if (!panel->activePresetId_.empty()) {
            panel->chat_->appendSystemNote(
                QStringLiteral("未找到预设 %1，使用当前激活预设。")
                    .arg(QString::fromStdString(presetId)));
            panel->runAgentWithPreset(panel->activePresetId_, userQuery);
        } else {
            panel->chat_->appendSystemNote(QStringLiteral("无可用预设。"));
        }
    }, Qt::QueuedConnection);
}

}  // namespace x64ai
