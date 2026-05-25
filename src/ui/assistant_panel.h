// ui/assistant_panel.h
//
// 主助手面板：左侧会话列表，右侧顶部模型条 + ChatView。
// M2 起承担会话持久化（SessionStore）与 RAG 检索拼接。
#pragma once

#include <QPointer>
#include <QWidget>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QToolButton;
class QMenu;

namespace x64ai {

class ChatView;
class SessionListWidget;
class AgentWorker;
struct AgentPreset;

class AssistantPanel : public QWidget {
    Q_OBJECT
public:
    static AssistantPanel* showInstance();
    static void            destroyInstance();

    // 反汇编右键 → AI 分析当前地址
    static void analyzeCurrentAddress();

    // 反汇编右键 → AI 追溯此函数调用链；va 来自当前选区。
    // 内部会确保面板已显示，并打开 TraceDialog 自动解析为函数。
    static void traceFunctionAt(uint64_t va);

    // 外部模块（如 TraceDialog）发起一次提问；自动显示并提交到当前会话。
    //   userVisible: 显示在对话区的标题/摘要
    //   userPrompt:  实际发给模型的内容（会经 RAG 增强）
    static void submitExternalPrompt(const QString& userVisible,
                                     const std::string& userPrompt);

    // M4.6f：右键 AI ▶ 子菜单 / 外部调用统一入口。
    //   presetId:  PresetStore 中的预设 id；不存在则降级到当前 activePreset
    //   userQuery: 用户原始问句（可空；用于 {{user}} 占位符）
    // 自动 showInstance + marshal 到 UI 线程。
    static void runPresetById(const std::string& presetId,
                              const QString&     userQuery);

    // 调试启停回调；plugin_callbacks 调用以驱动 ProjectContext 与 UI 刷新。
    static void onDebugStarted();
    static void onDebugStopped();

private:
    explicit AssistantPanel(QWidget* parent = nullptr);
    ~AssistantPanel() override;

    void onUserSubmitted(const QString& text);
    void refreshModelsAsync();
    void populateModels(const QStringList& ids, const QString& preferred);

    // M3.3: Provider 切换
    void onProviderChanged(int index);

    // 切到指定会话：清空 transcript，重新装载历史。
    void loadSession(int64_t sessionId);

    // 确保有一个可写会话；没有则自动创建。返回 0 表示当前无 store。
    int64_t ensureSession();

    // 发起一次 chat 调用；userVisible 为 true 时把 userPrompt 显示到对话区。
    // saveToStore=true 时把 user/assistant 消息持久化进当前会话。
    void sendChat(const std::string& systemPrompt,
                  const std::string& userPrompt,
                  const QString&     userVisibleText,
                  bool               saveToStore);

    // 在 userPrompt 前拼接 RAG 检索到的相关 chunk（若 store 与 PAT 可用）。
    std::string buildRagAugmentedPrompt(const std::string& userPrompt);

    // ===== M4.6d Agent 模式 =====
    // 用指定预设跑一轮 agent loop。
    //   presetId:  必须在 PresetStore；不存在则报错
    //   userQuery: 用户原始问句；模板里 {{user}} 用它替换
    // 已运行中再调会被忽略并提示。
    void runAgentWithPreset(const std::string& presetId,
                            const QString&     userQuery);

    // 重建 Agent 下拉菜单（PresetStore 增删改后调用）
    void rebuildAgentMenu();
    // 重建工具菜单
    void rebuildToolsMenu();
    // 设置 / 切换激活预设
    void setActivePreset(const std::string& presetId);
    // 取消当前运行
    void onCancelAgentClicked();
    // 打开预设管理器
    void openPresetManager();
    // 打开工具浏览器（S9++：只读工具一览，替代原 toolsMenu_ popup）
    void openToolsBrowser();

    // 切换 agent 运行 UI 状态（按钮 enabled / cancel 可见）
    void setAgentRunning(bool running);

    // 把 worker 信号接到 chat_/store
    void wireAgentWorker(AgentWorker* w,
                         int64_t sessionId,
                         const std::string& presetId);

    void refreshSessionPanel();

    // 同步底部状态栏（项目 / 模型 / RAG chunk 数）
    void updateStatusBar();

    // 刷新登录按钮文字与状态标签（已登录显示用户名，未登录显示"未登录"）。
    void refreshLoginStatus();
    // 启动 Device Flow 登录或登出。
    void onLoginClicked();
    // 打开启发式定位器对话框。
    void onLocatorClicked();
    // 打开调用链追溯对话框。
    void onTraceClicked();
    // 打开历史项目会话浏览器（M3.6）
    void onHistoryClicked();

    ChatView*           chat_        = nullptr;
    SessionListWidget*  sessions_    = nullptr;
    QComboBox*          providerBox_ = nullptr;  // M3.3: Copilot/DeepSeek 切换
    QComboBox*          modelBox_    = nullptr;
    QToolButton*        refreshBtn_  = nullptr;
    QToolButton*        locatorBtn_  = nullptr;
    QToolButton*        traceBtn_    = nullptr;
    QToolButton*        historyBtn_  = nullptr;
    QLabel*             loginStatus_ = nullptr;
    QToolButton*        loginBtn_    = nullptr;
    QLabel*             statusProject_ = nullptr;
    QLabel*             statusModel_   = nullptr;
    QLabel*             statusChunks_  = nullptr;

    // M4.6d
    QToolButton*        agentBtn_    = nullptr;   // split button：默认动作=用激活预设跑
    QMenu*              agentMenu_   = nullptr;
    QToolButton*        toolsBtn_    = nullptr;   // 多选复选菜单（暂禁，预设 enabledTools 优先）
    QMenu*              toolsMenu_   = nullptr;
    QPushButton*        cancelBtn_   = nullptr;
    QLabel*             agentStatusLabel_ = nullptr;  // 显示当前激活预设名 [+ 最近一轮 cache 命中率]

    std::string         activePresetId_;          // 当前激活预设（聊天框输入也用它）
    QPointer<AgentWorker> agentWorker_;
    std::shared_ptr<std::atomic<bool>> agentCancel_;  // 供 cancel 按钮触发
    bool                agentTerminalEventHandled_ = false;  // K-13: failed/maxIter 后跳过 finished 的冗余日志
    // G-2 (2026-05-25): 最近一轮 cache 状态（用于 agentStatusLabel_ 末尾拼接显示）
    QString             lastCacheStatus_;

    int64_t             currentSessionId_ = 0;
    QString             pendingModelForNewSession_;  // 用户已选模型但尚未创建会话时缓存

    static AssistantPanel* s_instance;
};

}  // namespace x64ai
