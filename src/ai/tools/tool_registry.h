// ai/tools/tool_registry.h
//
// ToolRegistry：进程内单例，集中注册所有 ITool 实现并提供：
//   - 列出所有可用工具的 ChatTool（喂给 IChatProvider）
//   - 按名 dispatch（参数 JSON 字符串 -> ToolResult JSON）
//   - 统一的 audit 日志、耗时统计、单次返回大小硬截断
//
// 注册时机：插件 pluginit 时调用 registerBuiltinTools()。
// 线程：注册一次后只读；dispatch 可以并发（工具实现自身要保证线程安全
//      ——本批工具都是同步调 x64dbg SDK，主进程内单调用即可）。
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ai/chat_provider.h"      // ChatTool / ToolCall
#include "ai/tools/tool.h"
#include "ai/tools/tool_context.h"

namespace x64ai {

class ToolRegistry {
public:
    static ToolRegistry& instance();

    // 注册一个工具（按 name 唯一；重名后注册覆盖前注册）。
    void registerTool(std::unique_ptr<ITool> tool);

    // 注册内置工具集（M4.4 实现完成后由它统一拉起所有 builtin tool）
    void registerBuiltinTools();

    // 列出全部工具的元信息，转成 ChatTool 喂给 IChatProvider。
    std::vector<ChatTool> listChatTools() const;

    // 单个 tool 是否存在
    bool has(const std::string& name) const;

    // 调用工具：argumentsJson 是 LLM 返回的字符串（OpenAI 协议本来就是 string）。
    // 内部：解析 JSON -> invoke -> 截断 -> 日志。
    // 永不抛异常；失败用 ToolResult.ok=false + error 表达。
    ToolResult dispatch(const std::string& name,
                        const std::string& argumentsJson,
                        ToolContext&       ctx);

    // 把 ToolResult 序列化为 OpenAI 协议要求的 tool 消息 content（字符串）。
    // 成功 -> data.dump()；失败 -> {"error": "..."} 的 JSON 字符串。
    static std::string serializeForLlm(const ToolResult& r);

private:
    ToolRegistry() = default;

    std::unordered_map<std::string, std::unique_ptr<ITool>> tools_;
};

}  // namespace x64ai
