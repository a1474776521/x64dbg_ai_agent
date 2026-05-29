// util/config.h
//
// 插件配置加载器（读取 %APPDATA%/x64dbg-ai-plugin/config.json）。
// 提供 Copilot 端点、Editor 头部伪装、默认模型等可覆盖项。
// 缺省值与 GitHub Copilot CLI 客户端保持一致，便于直接复用本机 OAuth。
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace x64ai {

struct CopilotEndpointConfig {
    // Chat / Models / Embeddings 基础 URL
    std::string apiBase = "https://api.githubcopilot.com";
    // OAuth -> Chat token 交换端点
    std::string tokenEndpoint = "https://api.github.com/copilot_internal/v2/token";

    // Editor-* 头：伪装为 VSCode Copilot Chat 客户端（与 device flow client_id 同源）。
    // copilot-cli / GitHubCopilotCLI 这些值在 /models 端点会被拒（unknown Copilot-Integration-Id）。
    std::string editorVersion        = "vscode/1.95.0";
    std::string editorPluginVersion  = "copilot-chat/0.22.0";
    std::string copilotIntegrationId = "vscode-chat";
    std::string userAgent            = "GitHubCopilotChat/0.22.0";
    std::string openaiIntent         = "conversation-other";
};

struct DeepSeekEndpointConfig {
    // OpenAI 兼容的基础 URL（chat/completions、models）
    std::string apiBase = "https://api.deepseek.com/v1";
    // 默认模型
    std::string defaultModel = "deepseek-chat";
};

struct AppConfig {
    CopilotEndpointConfig copilot;
    DeepSeekEndpointConfig deepseek;
    // 当前 Provider："copilot" | "deepseek"
    std::string           provider     = "copilot";
    std::string           defaultModel = "gpt-4o-mini";
    int                   httpTimeoutMs = 60000;
    // 流式响应超时（毫秒）：用于 chat/completions 的 SSE 长连接。
    // DeepSeek reasoner / 大模型推理可能持续几分钟，默认 10 分钟。
    int                   streamTimeoutMs = 600000;
    // 流式低速阈值（秒）：连续 N 秒未收到任何字节即视为断流并中断。
    // 防止真正的网络挂死把整条请求拖到 streamTimeoutMs 才退出。
    int                   streamLowSpeedSec = 30;

    // run_dbg_command 工具的【附加白名单】，与代码内置的硬白名单求并集。
    // 内置白名单见 debug_write_tools.cpp::dbgCmdWhitelist() 默认集；
    // 这里允许用户在 config.json 用 "extra_dbg_cmd_whitelist": ["bpdll","bcdll"] 自加命令。
    // 命令名按 x64dbg 命令首 token 写，大小写不敏感（内部统一转小写比较）。
    // 注意：这只能添加，不能从默认集移除（移除得改代码，避免误关键护栏）。
    std::vector<std::string> extraDbgCmdWhitelist;

    // K-33：用户自定义【跳过 5s confirm 弹窗】的写工具列表。
    // 默认所有 Write 类工具都弹窗；加入本列表的工具会被自动批准（仍写 audit log）。
    // 黑名单内的工具（run_dbg_command / start_debug / attach_debug / stop_debug / patch_file）
    // 即使加入也会被强制 confirm —— 详见 ai/tools/confirm_policy.h::confirmHardEnforced。
    // 工具名小写、和 ITool::name() 完全匹配（如 "set_label"）。修改后需重启插件生效。
    std::vector<std::string> autoApproveTools;

    // K-35：agent 编排增强开关 —— tool retry。
    // 当某次 tool dispatch 失败（ok=false）且错误属于"瞬时/可恢复"类
    // （网络/超时/HTTP 5xx/connection/embedding failed），AgentLoop 会自动退避重试。
    // 仅对【非 Write 类】工具重试（Write 已确认的副作用不能重复触发）；
    // 参数错误 / 校验失败 / 业务逻辑失败不重试（重试无意义）。
    // toolRetryMax=0 等价于关闭。默认开启、最多重试 1 次。
    bool toolRetryEnabled = true;
    int  toolRetryMax     = 1;     // 额外重试次数（0=不重试）；上限 3

    // K-35：agent 编排增强开关 —— auto-RAG 注入。
    // 每次 agent run 开始前，用首条 user 消息做一次向量检索（embed + searchSimilar），
    // 把 top-K 历史分析 chunks 作为一条 system 上下文消息注入对话，
    // 免去 LLM 必须显式调 rag_search 才能拿到历史的问题。
    // 需要当前 session 有 SessionStore 且 GitHub Models PAT 可用；否则静默跳过。
    // 默认开启、top-K=4。
    bool autoRagInjectEnabled = true;
    int  autoRagTopK          = 4;  // 1-16

    // K-36：agent 编排增强 —— 只读工具并行执行。
    // 当 LLM 一轮回吐的【一批 tool_calls 全部为 Read 类】时，AgentLoop 用线程池
    // 并发 dispatch（最多 parallelReadMax 个同时），把多个只读探查从串行压成并行。
    // 只要该批里有任何一个 DbgControl / Write 工具，整批立即回退串行（保证 confirm
    // 弹窗顺序 + audit 顺序 + 写副作用时序不被打乱）。
    // 默认开启、并发上限 4。parallelReadMax<=1 等价于关闭。
    bool parallelReadEnabled = true;
    int  parallelReadMax     = 4;   // 同时并发的只读工具数；1=串行；上限 8

    // K-36：agent 编排增强 —— 上下文压缩。
    // 每轮 streamChat 前，按"字符数/4"粗估当前 messages 的累计 token；
    // 若超过【当前模型上下文窗口 * contextCompressThresholdPct%】，则把最老的
    // 若干【整轮】（一条 assistant + 其后紧跟的全部 tool 消息）本地折叠成一条
    // system 摘要消息（不调 LLM）。整轮折叠保证 assistant↔tool_call_id 配对不被
    // 拆散（否则 provider 会 400）。system / 最近若干轮 / 末轮永不压缩。
    // 默认开启、阈值 75%。contextCompressEnabled=false 关闭。
    bool contextCompressEnabled       = true;
    int  contextCompressThresholdPct  = 75;  // 占模型窗口百分比触发；10-95
    int  contextCompressKeepRounds    = 3;   // 末尾保留不压缩的轮数；>=1

    // K-39：system tools（fs_read_file / fs_write_file / fs_create_file / shell_cmd / shell_pwsh）
    //
    // 文件系统白名单。**仅这些目录内的路径**可被 fs_* 工具读写。
    // 配置时支持以下占位符（loadConfig 会就地展开）：
    //   {plugin_workdir}   ->  pluginRootDir()       即 %APPDATA%/x64dbg-ai-plugin/
    //   {plugin_temp}      ->  %TEMP%/x64dbg-ai-plugin/
    //   {debuggee_dir}     ->  当前主调试模块所在目录（运行时按 ToolContext 解析）
    //   {user_home}        ->  %USERPROFILE%/
    // 留空 → 启用默认集合：[{plugin_workdir}, {plugin_temp}]（按 Q4=a，不含 debuggee_dir）。
    //
    // 路径校验规则（见 system_tools.cpp）：
    //   - 绝对路径化 + 前缀匹配白名单目录（含末尾分隔符）
    //   - 拒绝含 ".." 的原始路径（防绕过）
    //   - 拒绝 reparse point（junction/symlink）
    //   - 拒绝 UNC / 设备名（NUL/CON/PRN/AUX/COM*/LPT*）
    std::vector<std::string> fsAllowedDirs;

    // fs_read_file：单次最多读取字节数。默认 64KB，上限 4MB。
    int fsReadMaxBytesDefault = 65536;
    int fsReadMaxBytesCap     = 4194304;

    // fs_write_file / fs_create_file：单次 content 字节数上限。默认 = cap = 4MB。
    int fsWriteMaxBytesCap    = 4194304;

    // shell_cmd / shell_pwsh：子进程执行超时。
    // 默认 30s，上限 5min（与 K-37 run_continue 一致）。
    int shellTimeoutMsDefault = 30000;
    int shellTimeoutMsCap     = 300000;

    // shell_cmd / shell_pwsh：stdout / stderr 各自的字节数上限（捕获后超出截断 + truncated=true）。
    // 默认 64KB，上限 1MB。
    int shellStdoutMaxBytesDefault = 65536;
    int shellStdoutMaxBytesCap     = 1048576;
    int shellStderrMaxBytesDefault = 65536;
    int shellStderrMaxBytesCap     = 1048576;
};

class Config {
public:
    static Config& instance();

    // 首次访问时自动加载；可通过 reload() 强制重新读取。
    const AppConfig& get();
    void             reload();

private:
    Config() = default;
    void loadLocked();

    std::mutex mtx_;
    bool       loaded_ = false;
    AppConfig  data_;
};

}  // namespace x64ai
