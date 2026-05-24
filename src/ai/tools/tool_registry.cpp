// ai/tools/tool_registry.cpp
#include "ai/tools/tool_registry.h"

#include <chrono>

#include "ai/tools/builtin_tools.h"
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
