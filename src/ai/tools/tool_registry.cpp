// ai/tools/tool_registry.cpp
#include "ai/tools/tool_registry.h"

#include <chrono>

#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool_context.h"
#include "util/logging.h"

namespace x64ai {

ToolRegistry& ToolRegistry::instance()
{
    static ToolRegistry inst;
    return inst;
}

void ToolRegistry::registerTool(std::unique_ptr<ITool> tool)
{
    if (!tool) return;
    const std::string name = tool->name();
    if (name.empty()) {
        XAI_LOG_WARN("ToolRegistry: skip tool with empty name");
        return;
    }
    if (tools_.count(name)) {
        XAI_LOG_WARN("ToolRegistry: override existing tool '{}'", name);
    }
    tools_[name] = std::move(tool);
    XAI_LOG_INFO("ToolRegistry: registered '{}'", name);
}

void ToolRegistry::registerBuiltinTools()
{
    if (!tools_.empty()) {
        XAI_LOG_WARN("ToolRegistry::registerBuiltinTools(): already populated ({} tools), skip",
                     tools_.size());
        return;
    }
    registerBasicReadTools(*this);
    registerStaticAnalysisTools(*this);
    registerDynamicAndContextTools(*this);
    registerDebugControlTools(*this);
    registerDebugWriteTools(*this);
    registerDataWriteTools(*this);
    XAI_LOG_INFO("ToolRegistry::registerBuiltinTools(): total {} tools", tools_.size());
}

bool ToolRegistry::has(const std::string& name) const
{
    return tools_.count(name) > 0;
}

std::vector<ChatTool> ToolRegistry::listChatTools() const
{
    std::vector<ChatTool> out;
    out.reserve(tools_.size());
    for (const auto& [name, tool] : tools_) {
        ChatTool t;
        t.name = name;
        t.description = tool->description();
        try {
            t.parametersJson = tool->parametersSchema().dump();
        } catch (const std::exception& e) {
            XAI_LOG_WARN("ToolRegistry: tool '{}' schema serialize fail: {}", name, e.what());
            t.parametersJson = R"({"type":"object","properties":{}})";
        }
        out.push_back(std::move(t));
    }
    return out;
}

ToolResult ToolRegistry::dispatch(const std::string& name,
                                  const std::string& argumentsJson,
                                  ToolContext&       ctx)
{
    ToolResult r;
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        r.ok = false;
        r.error = "tool not found: " + name;
        XAI_LOG_WARN("ToolRegistry::dispatch: {} (no such tool)", r.error);
        return r;
    }

    ITool& tool = *it->second;

    // 解析参数 JSON
    nlohmann::json args = nlohmann::json::object();
    if (!argumentsJson.empty()) {
        try {
            args = nlohmann::json::parse(argumentsJson);
            if (!args.is_object()) {
                // OpenAI 协议保证 arguments 是 object string；非 object 视为错误
                ToolResult bad;
                bad.ok = false;
                bad.error = "arguments must be a JSON object";
                XAI_LOG_WARN("ToolRegistry::dispatch '{}': non-object args: {}",
                             name, argumentsJson);
                return bad;
            }
        } catch (const std::exception& e) {
            ToolResult bad;
            bad.ok = false;
            bad.error = std::string("invalid arguments JSON: ") + e.what();
            XAI_LOG_WARN("ToolRegistry::dispatch '{}': parse fail: {}; raw={}",
                         name, e.what(), argumentsJson);
            return bad;
        }
    }

    // S3-D：根据风险分档决定是否 confirm + 写 audit。
    // Read 工具：直接 invoke。
    // DbgControl / Write 工具：必须写 audit（开始 + 结束各一条），
    //                          Write 还要先弹 confirm（除非 requiresUserConfirmation()==false 显式豁免）。
    const ToolCategory cat       = tool.category();
    const bool         isWrite   = (cat == ToolCategory::Write);
    const bool         isControl = (cat == ToolCategory::DbgControl);
    const bool         needAudit = isWrite || isControl;
    const bool         needConfirm = isWrite && tool.requiresUserConfirmation();

    if (needConfirm) {
        if (!ctx.confirmCallback) {
            r.ok = false;
            r.error = "write tool requires user confirmation but no UI is attached";
            XAI_LOG_WARN("ToolRegistry::dispatch '{}': no confirmCallback; deny", name);
            // 仍要落 audit
            nlohmann::json a = {
                {"ts",     std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count()},
                {"tool",   name},
                {"category", toolCategoryName(cat)},
                {"args",   args},
                {"phase",  "denied_no_ui"},
                {"sha",    ctx.targetSha},
                {"session", ctx.sessionId},
            };
            try { auditLog()->info(a.dump()); } catch (...) {}
            return r;
        }
        // 摘要：取工具的 description() 前一行 + 关键参数提示
        std::string summary = tool.description();
        if (auto nl = summary.find('\n'); nl != std::string::npos) summary.resize(nl);
        if (summary.size() > 160) summary.resize(160);

        std::string argsPretty;
        try { argsPretty = args.dump(2); } catch (...) { argsPretty = "(dump failed)"; }

        const bool allowed = ctx.confirmCallback(name, summary, argsPretty);

        // 不管 allow / deny 都先记 audit
        try {
            nlohmann::json a = {
                {"ts",     std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count()},
                {"tool",   name},
                {"category", toolCategoryName(cat)},
                {"args",   args},
                {"phase",  allowed ? "confirmed" : "denied_by_user"},
                {"sha",    ctx.targetSha},
                {"session", ctx.sessionId},
            };
            auditLog()->info(a.dump());
        } catch (...) {}

        if (!allowed) {
            r.ok = false;
            r.error = "user denied tool execution";
            XAI_LOG_INFO("tool '{}' denied by user", name);
            return r;
        }
    } else if (needAudit) {
        // DbgControl 或 Write-无 confirm：开始前留痕
        try {
            nlohmann::json a = {
                {"ts",     std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count()},
                {"tool",   name},
                {"category", toolCategoryName(cat)},
                {"args",   args},
                {"phase",  "begin"},
                {"sha",    ctx.targetSha},
                {"session", ctx.sessionId},
            };
            auditLog()->info(a.dump());
        } catch (...) {}
    }

    // 调用 + 计时
    const auto t0 = std::chrono::steady_clock::now();
    try {
        r = tool.invoke(args, ctx);
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = std::string("tool threw: ") + e.what();
        XAI_LOG_ERROR("ToolRegistry::dispatch '{}' threw: {}", name, e.what());
    } catch (...) {
        r.ok = false;
        r.error = "tool threw unknown exception";
        XAI_LOG_ERROR("ToolRegistry::dispatch '{}' threw unknown", name);
    }
    const auto t1 = std::chrono::steady_clock::now();
    r.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);

    // S3-D：写/控制工具结束后再落一条 audit（带 ok + error/data 摘要）
    if (needAudit) {
        try {
            std::string dataSnippet;
            try { dataSnippet = r.data.dump(); } catch (...) { dataSnippet = "(dump failed)"; }
            if (dataSnippet.size() > 512) dataSnippet.resize(512);
            nlohmann::json a = {
                {"ts",     std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count()},
                {"tool",   name},
                {"category", toolCategoryName(cat)},
                {"phase",  "end"},
                {"ok",     r.ok},
                {"error",  r.error},
                {"data_snippet", dataSnippet},
                {"elapsed_ms", r.elapsed.count()},
            };
            auditLog()->info(a.dump());
        } catch (...) {}
    }

    // 大小截断（粗略：按 dump 后的 string size）
    if (r.ok) {
        try {
            std::string s = r.data.dump();
            const std::size_t cap = tool.maxResultBytes();
            if (s.size() > cap) {
                // 截断为 string，把"被截断"信息塞回 data
                nlohmann::json j = {
                    {"truncated", true},
                    {"original_bytes", s.size()},
                    {"max_bytes", cap},
                    {"preview", s.substr(0, cap)},
                };
                r.data = std::move(j);
                r.truncatedTo = cap;
                XAI_LOG_WARN("ToolRegistry::dispatch '{}': truncated {} -> {} bytes",
                             name, s.size(), cap);
            }
        } catch (const std::exception& e) {
            r.ok = false;
            r.error = std::string("result serialize fail: ") + e.what();
        }
    }

    XAI_LOG_INFO("tool '{}' ok={} elapsed={}ms truncated={}",
                 name, r.ok, r.elapsed.count(), r.truncatedTo);
    return r;
}

std::string ToolRegistry::serializeForLlm(const ToolResult& r)
{
    if (r.ok) {
        try { return r.data.dump(); }
        catch (...) { return R"({"error":"serialize failed"})"; }
    }
    nlohmann::json j = {{"error", r.error}};
    return j.dump();
}

}  // namespace x64ai
