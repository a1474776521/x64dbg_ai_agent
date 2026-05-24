// ai/tools/annotation_tools.cpp
//
// S6-B/C：标签 + 注释（"沉淀"类工具）。让 AI 把分析结果写回 x64dbg，
// 跨会话累积，下次打开同一程序自动看到 AI 命名/注释。
//
//   set_label / get_label / list_labels       Script::Label::*
//   set_comment / get_comment / list_comments Script::Comment::*
//
// 共性：
//   - set_* 是 Write（5s confirm + audit），get_* / list_* 是 Read（不弹窗、不审计）。
//   - text 上限 MAX_LABEL_SIZE - 1（256 - 1 = 255 字节，UTF-8 自动接受）。
//   - delete 通过 set_label("") 表达：x64dbg SDK 用空文本删除标签/注释。
//     这样 agent 不需要单独 delete_label 工具，减少同义工具数量。
#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <Windows.h>
#include "bridgemain.h"
#include "bridgelist.h"
#include "_scriptapi_label.h"
#include "_scriptapi_comment.h"

#include "util/logging.h"

namespace x64ai {

namespace {

std::string formatHexU64(std::uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

bool parseVa(const nlohmann::json& args, const char* key, std::uint64_t& out, std::string& err)
{
    if (!args.contains(key)) { err = std::string("'") + key + "' is required"; return false; }
    const auto& v = args[key];
    if (v.is_number_unsigned()) { out = v.get<std::uint64_t>(); return true; }
    if (v.is_number_integer())  { out = static_cast<std::uint64_t>(v.get<std::int64_t>()); return true; }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        try {
            std::size_t pos = 0;
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                out = std::stoull(s.substr(2), &pos, 16);
                if (pos + 2 == s.size()) return true;
            } else {
                out = std::stoull(s, &pos, 0);
                if (pos == s.size()) return true;
            }
        } catch (...) {}
    }
    err = std::string("invalid '") + key + "': expected number or hex string";
    return false;
}

}  // namespace

// ============= S6-B label =============

class SetLabelTool : public ITool {
public:
    std::string name() const override { return "set_label"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Persistently name an address in x64dbg (visible in disasm/dump). "
               "Pass text=\"\" to delete the label. Max 255 bytes UTF-8.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"address", {{"type","string"},{"description","VA, decimal or 0x hex"}}},
                {"text",    {{"type","string"},{"description","Label text; empty string deletes"}}},
            }},
            {"required", nlohmann::json::array({"address","text"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        std::uint64_t va = 0; std::string e;
        if (!parseVa(args, "address", va, e)) { r.ok=false; r.error=e; return r; }
        if (!args.contains("text") || !args["text"].is_string()) {
            r.ok=false; r.error="'text' required (string; empty to delete)"; return r;
        }
        const auto text = args["text"].get<std::string>();
        if (text.size() >= MAX_LABEL_SIZE) {
            r.ok=false; r.error="label text too long (max 255 bytes)"; return r;
        }
        if (text.empty()) {
            const bool ok = Script::Label::Delete(static_cast<duint>(va));
            r.ok = ok;
            if (!ok) r.error = "Script::Label::Delete failed at " + formatHexU64(va);
            else     r.data = {{"action","delete_label"},{"address", formatHexU64(va)}};
            return r;
        }
        const bool ok = Script::Label::Set(static_cast<duint>(va), text.c_str(), /*manual=*/true, /*temporary=*/false);
        r.ok = ok;
        if (!ok) r.error = "Script::Label::Set failed at " + formatHexU64(va);
        else     r.data = {{"action","set_label"},{"address", formatHexU64(va)},{"text", text}};
        return r;
    }
};

class GetLabelTool : public ITool {
public:
    std::string name() const override { return "get_label"; }
    std::string description() const override
    {
        return "Get the label at an address (empty if none). Reads either manual or auto label.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"address", {{"type","string"},{"description","VA, decimal or 0x hex"}}},
            }},
            {"required", nlohmann::json::array({"address"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        std::uint64_t va = 0; std::string e;
        if (!parseVa(args, "address", va, e)) { r.ok=false; r.error=e; return r; }
        char text[MAX_LABEL_SIZE] = {0};
        const bool ok = Script::Label::Get(static_cast<duint>(va), text);
        r.ok = true;
        r.data = {{"address", formatHexU64(va)},
                  {"text",    ok ? std::string(text) : std::string("")},
                  {"has_label", ok && text[0] != '\0'}};
        return r;
    }
};

class ListLabelsTool : public ITool {
public:
    std::string name() const override { return "list_labels"; }
    std::string description() const override
    {
        return "List all labels in all modules (module + rva + text + manual flag). "
               "Result is paged in memory; if too large, use get_label per address instead.";
    }
    nlohmann::json parametersSchema() const override
    {
        return { {"type","object"}, {"properties", nlohmann::json::object()} };
    }
    std::size_t maxResultBytes() const override { return 128 * 1024; }
    ToolResult invoke(const nlohmann::json& /*args*/, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        BridgeList<Script::Label::LabelInfo> list;
        if (!Script::Label::GetList(&list)) {
            r.ok = false; r.error = "Script::Label::GetList failed"; return r;
        }
        nlohmann::json arr = nlohmann::json::array();
        const int n = list.Count();
        for (int i = 0; i < n; ++i) {
            const auto& it = list[i];
            arr.push_back({
                {"module", it.mod},
                {"rva",    formatHexU64(static_cast<std::uint64_t>(it.rva))},
                {"text",   it.text},
                {"manual", it.manual},
            });
        }
        r.ok = true;
        r.data = {{"count", n}, {"labels", std::move(arr)}};
        return r;
    }
};

// ============= S6-C comment =============

class SetCommentTool : public ITool {
public:
    std::string name() const override { return "set_comment"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Set the line comment at an address (visible in disasm view). "
               "Pass text=\"\" to delete the comment. Max 255 bytes UTF-8.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"address", {{"type","string"},{"description","VA, decimal or 0x hex"}}},
                {"text",    {{"type","string"},{"description","Comment text; empty to delete"}}},
            }},
            {"required", nlohmann::json::array({"address","text"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        std::uint64_t va = 0; std::string e;
        if (!parseVa(args, "address", va, e)) { r.ok=false; r.error=e; return r; }
        if (!args.contains("text") || !args["text"].is_string()) {
            r.ok=false; r.error="'text' required (string; empty to delete)"; return r;
        }
        const auto text = args["text"].get<std::string>();
        if (text.size() >= MAX_LABEL_SIZE) {
            r.ok=false; r.error="comment text too long (max 255 bytes)"; return r;
        }
        if (text.empty()) {
            const bool ok = Script::Comment::Delete(static_cast<duint>(va));
            r.ok = ok;
            if (!ok) r.error = "Script::Comment::Delete failed at " + formatHexU64(va);
            else     r.data = {{"action","delete_comment"},{"address", formatHexU64(va)}};
            return r;
        }
        const bool ok = Script::Comment::Set(static_cast<duint>(va), text.c_str(), /*manual=*/true);
        r.ok = ok;
        if (!ok) r.error = "Script::Comment::Set failed at " + formatHexU64(va);
        else     r.data = {{"action","set_comment"},{"address", formatHexU64(va)},{"text", text}};
        return r;
    }
};

class GetCommentTool : public ITool {
public:
    std::string name() const override { return "get_comment"; }
    std::string description() const override
    {
        return "Get the comment at an address (empty if none).";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"address", {{"type","string"},{"description","VA, decimal or 0x hex"}}},
            }},
            {"required", nlohmann::json::array({"address"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        std::uint64_t va = 0; std::string e;
        if (!parseVa(args, "address", va, e)) { r.ok=false; r.error=e; return r; }
        char text[MAX_LABEL_SIZE] = {0};
        const bool ok = Script::Comment::Get(static_cast<duint>(va), text);
        r.ok = true;
        r.data = {{"address", formatHexU64(va)},
                  {"text",    ok ? std::string(text) : std::string("")},
                  {"has_comment", ok && text[0] != '\0'}};
        return r;
    }
};

class ListCommentsTool : public ITool {
public:
    std::string name() const override { return "list_comments"; }
    std::string description() const override
    {
        return "List all comments in all modules (module + rva + text + manual flag).";
    }
    nlohmann::json parametersSchema() const override
    {
        return { {"type","object"}, {"properties", nlohmann::json::object()} };
    }
    std::size_t maxResultBytes() const override { return 128 * 1024; }
    ToolResult invoke(const nlohmann::json& /*args*/, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        BridgeList<Script::Comment::CommentInfo> list;
        if (!Script::Comment::GetList(&list)) {
            r.ok = false; r.error = "Script::Comment::GetList failed"; return r;
        }
        nlohmann::json arr = nlohmann::json::array();
        const int n = list.Count();
        for (int i = 0; i < n; ++i) {
            const auto& it = list[i];
            arr.push_back({
                {"module", it.mod},
                {"rva",    formatHexU64(static_cast<std::uint64_t>(it.rva))},
                {"text",   it.text},
                {"manual", it.manual},
            });
        }
        r.ok = true;
        r.data = {{"count", n}, {"comments", std::move(arr)}};
        return r;
    }
};

void registerAnnotationTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<SetLabelTool>());
    reg.registerTool(std::make_unique<GetLabelTool>());
    reg.registerTool(std::make_unique<ListLabelsTool>());
    reg.registerTool(std::make_unique<SetCommentTool>());
    reg.registerTool(std::make_unique<GetCommentTool>());
    reg.registerTool(std::make_unique<ListCommentsTool>());
}

}  // namespace x64ai
