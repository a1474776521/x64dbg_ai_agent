// ai/tools/tool.h
//
// ITool：单个 agent 工具的抽象。
//
// 设计原则：
//   - 工具完全无状态（必要状态走 ToolContext）
//   - 输入/输出均 JSON（nlohmann::json），与 OpenAI function calling 对齐
//   - 工具自身不抛异常；用 ToolResult.ok 表达成功/失败，便于 agent loop 回吐给 LLM
//   - 单次返回有大小硬上限（默认 8 KB，子类可覆盖）
#pragma once

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "ai/tools/tool_policy.h"

namespace x64ai {

struct ToolContext;  // 见 tool_context.h（前置声明，避免循环依赖）

struct ToolResult {
    bool           ok = true;
    nlohmann::json data;          // 成功时的返回（任意 JSON）
    std::string    error;         // 失败时的人类可读说明（也会回吐给 LLM）
    std::chrono::milliseconds elapsed{0};
    std::size_t    truncatedTo = 0;  // 若被截断，给出最终大小（bytes），0 表示未截断
};

class ITool {
public:
    virtual ~ITool() = default;

    // === 元信息（用于 ChatTool / 给 LLM 看的 schema） ===
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;

    // JSON Schema（object 顶层），形如：
    //   { "type":"object","properties":{...},"required":[...] }
    virtual nlohmann::json parametersSchema() const = 0;

    // 写类工具需要 UI 确认时返回 true；本批工具全部只读，默认 false
    virtual bool requiresUserConfirmation() const { return false; }

    // S3-A：风险分档。默认 Read；写工具子类必须覆盖为 Write，
    // 调试控制工具覆盖为 DbgControl。用于 ToolRegistry::dispatch 决定 audit / confirm。
    virtual ToolCategory category() const { return ToolCategory::Read; }

    // 单次返回字节硬上限（基础读取类默认 64KB，否则 8KB）
    virtual std::size_t maxResultBytes() const { return 8 * 1024; }

    // === 执行 ===
    // args 已校验过基本结构（与 schema 一致），实现里仍要做范围检查。
    // ctx 提供对调试器 / 项目状态 / SessionStore 的按需访问。
    virtual ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) = 0;
};

}  // namespace x64ai
