// ai/chat_provider.h
//
// IChatProvider：统一抽象，使 Copilot / DeepSeek / OpenRouter 等 LLM 后端可热切换。
//
// 设计原则：
//   - ChatRequest / ChatMessage / ChatStreamCallbacks 在此定义（OpenAI 风格）
//   - 接口纯虚 + 单例由各实现类持有，ProviderManager 仅负责选择当前实现
//   - streamChat 同步阻塞，调用方负责放后台线程
//
// M4 扩展：
//   - 增加 OpenAI 风格 function calling 字段（tools / tool_calls / tool role 消息）
//   - 旧调用方不传 tools 时行为与之前完全一致（向后兼容）
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace x64ai {

// ===== Tool 描述（发送给 LLM 用） =====
//
// schema 是 JSON Schema 字符串，描述 function 的参数。
// 我们直接持 JSON 文本而不解析，避免在头文件里暴露 nlohmann/json。
struct ChatTool {
    std::string name;            // 如 "get_disasm"
    std::string description;     // 给 LLM 看的功能说明（英文）
    std::string parametersJson;  // JSON Schema 的字符串（object 顶层）
    std::string descriptionZh;   // 中文描述（仅 UI 显示用；空时 UI 用 description 兜底）
};

// ===== LLM 返回的工具调用（流式拼装完成后向上回吐） =====
struct ToolCall {
    std::string id;           // OpenAI 提供的 tool_call_id，回填 tool 消息时用
    std::string name;         // function 名
    std::string argumentsJson;// JSON 字符串（参数）
};

struct ChatMessage {
    // role: "system" | "user" | "assistant" | "tool"
    std::string role;
    std::string content;

    // assistant 携带的 tool_calls（要求模型调工具时）
    std::vector<ToolCall> toolCalls;

    // role=="tool" 时必填：对应的 tool_call_id 与（可选）function 名
    std::string toolCallId;
    std::string toolName;

    // DeepSeek thinking 模型（reasoner 等）：assistant 上一轮的 reasoning_content。
    // M-1 (2026-05-24) 实测：deepseek-reasoner 接受"回传"与"剥离"两种形式，
    // 均返回 HTTP 200，文本质量相当。本字段保留是为了：
    //   1) 向下兼容：若官方未来收紧协议要求回传，已就位；
    //   2) 长 reasoning 可被本插件 UI 折叠展示（onReasoningDelta）。
    // 其他 provider 忽略即可。注：长会话会累积 token，AgentLoop 可裁老轮次。
    std::string reasoningContent;
};

struct ChatRequest {
    std::string              model;
    std::vector<ChatMessage> messages;
    bool                     stream      = true;
    double                   temperature = 0.2;
    int                      maxTokens   = 0;  // 0 -> 不传

    // === M4: 工具调用 ===
    // 为空则与旧行为一致（不发 tools 字段）。
    std::vector<ChatTool>    tools;
    // 取值: "" / "auto" / "none" / "required"
    // 为空时不发；一般 agent 模式建议 "auto"。
    std::string              toolChoice;
};

struct ChatStreamCallbacks {
    std::function<void(std::string_view delta)> onDelta;
    std::function<void(std::string error)>      onError;
    std::function<void()>                       onDone;

    // === M4: 工具调用回吐 ===
    // 流结束（stop_reason=tool_calls）时一次性回吐拼装完成的 tool_calls。
    // 没要求调工具时不触发。
    std::function<void(std::vector<ToolCall> calls)> onToolCalls;

    // DeepSeek thinking 模型的推理过程增量。与 onDelta 分开传递，
    // 既可在 UI 里折叠显示，也便于 AgentLoop 累积后回传给下一轮 API。
    std::function<void(std::string_view delta)> onReasoningDelta;
};

// Provider 标识；持久化到 provider.txt（fallback: config.json 的 "provider" 字段）。
enum class ProviderKind {
    Copilot   = 0,
    DeepSeek  = 1,
};

inline const char* providerKindToString(ProviderKind k)
{
    switch (k) {
    case ProviderKind::Copilot:  return "copilot";
    case ProviderKind::DeepSeek: return "deepseek";
    }
    return "copilot";
}

inline ProviderKind providerKindFromString(const std::string& s)
{
    if (s == "deepseek") return ProviderKind::DeepSeek;
    return ProviderKind::Copilot;
}

class IChatProvider {
public:
    virtual ~IChatProvider() = default;

    virtual ProviderKind kind() const = 0;
    virtual std::string  displayName() const = 0;

    virtual bool isAuthenticated(std::string* outReason = nullptr) const = 0;
    virtual std::vector<std::string> listModels() = 0;
    virtual void streamChat(const ChatRequest& req, const ChatStreamCallbacks& cb) = 0;
    virtual std::string defaultModel() const = 0;
};

}  // namespace x64ai
