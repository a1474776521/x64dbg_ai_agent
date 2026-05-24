// ai/tools/patch_misc_tools.cpp
//
// S7-G list_patches / restore_patch  - 补丁审计与撤销
// S7-H format_with_dbg               - 走 x64dbg StringFormatInline 渲染模板
// S7-I gui_focus_disasm / gui_focus_dump - 引导用户视线（DbgControl 类，无副作用）
//
// 补丁说明：
//   PatchEnum(NULL, &cbsize) 第一次查询所需字节，再分配后第二次填充。
//   DBGPATCHINFO = {mod[MAX_MODULE_SIZE], addr, oldbyte, newbyte}（_dbgfunctions.h:10-16）
//   每条 = 单字节差异，N 字节补丁会出现 N 条记录。
//
// 模板：
//   StringFormatInline 支持 x64dbg 表达式 + 内联函数；常用 "{x:[rax+8]}" 取内存。
//   我们预分配 4 KB 输出（够 99% 的模板），失败时报错。
//
// GUI Focus：
//   GuiDisasmAt(addr, cip)：滚动反汇编视图到 addr，cip 是当前 EIP（仅高亮用，我们填 addr）。
//   GuiDumpAt(va)：滚动 dump 视图到 va。
//   无副作用 / 无确认；归类为 DbgControl（与 wait_for_event 同级）。
#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <Windows.h>
#include "bridgemain.h"
#include "_dbgfunctions.h"

#include "util/logging.h"

namespace x64ai {

namespace {

constexpr std::size_t kFormatBufSize = 4096;
constexpr std::size_t kMaxPatches    = 4096;  // 安全上限

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

bool iequals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) return false;
    }
    return true;
}

}  // namespace

// ============= S7-G list_patches =============
class ListPatchesTool : public ITool {
public:
    std::string name() const override { return "list_patches"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "List all byte-level patches applied so far. Optional 'module' filter "
               "(case-insensitive substring). Each entry = one byte diff. Capped at 4096.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"module", {{"type","string"},{"description","Filter by module name substring (optional)"}}},
            }},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        const auto* fns = DbgFunctions();
        if (!fns || !fns->PatchEnum) { r.ok=false; r.error="DbgFunctions->PatchEnum is null"; return r; }

        std::string filter;
        if (args.contains("module") && args["module"].is_string()) {
            filter = args["module"].get<std::string>();
        }

        // 第一次：查询所需 cbsize
        std::size_t cbsize = 0;
        if (!fns->PatchEnum(nullptr, &cbsize)) {
            r.ok=false; r.error="PatchEnum(NULL,&size) failed"; return r;
        }
        if (cbsize == 0) {
            r.ok=true;
            r.data = {{"count",0},{"total",0},{"truncated",false},{"patches", nlohmann::json::array()}};
            return r;
        }
        const std::size_t count = cbsize / sizeof(DBGPATCHINFO);
        std::vector<DBGPATCHINFO> buf(count);
        if (!fns->PatchEnum(buf.data(), nullptr)) {
            r.ok=false; r.error="PatchEnum(data,NULL) failed"; return r;
        }

        nlohmann::json arr = nlohmann::json::array();
        std::size_t emitted = 0;
        for (const auto& p : buf) {
            if (!filter.empty()) {
                const std::string mod = p.mod;
                bool match = false;
                // 子串大小写不敏感
                if (mod.size() >= filter.size()) {
                    for (std::size_t i = 0; i + filter.size() <= mod.size(); ++i) {
                        if (iequals(mod.substr(i, filter.size()), filter)) { match = true; break; }
                    }
                }
                if (!match) continue;
            }
            if (emitted >= kMaxPatches) break;
            arr.push_back({
                {"module",  p.mod},
                {"address", formatHexU64(static_cast<std::uint64_t>(p.addr))},
                {"old",     static_cast<unsigned>(p.oldbyte)},
                {"new",     static_cast<unsigned>(p.newbyte)},
            });
            ++emitted;
        }

        XAI_LOG_INFO("list_patches: total={} emitted={} filter=\"{}\"",
                     static_cast<unsigned long long>(count),
                     static_cast<unsigned long long>(emitted), filter.c_str());
        r.ok=true;
        r.data = {
            {"total",     count},
            {"count",     emitted},
            {"truncated", emitted >= kMaxPatches},
            {"filter",    filter},
            {"patches",   std::move(arr)},
        };
        return r;
    }
};

// ============= S7-G restore_patch =============
class RestorePatchTool : public ITool {
public:
    std::string name() const override { return "restore_patch"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Restore the original byte at VA, undoing a single-byte patch. "
               "Returns false if no patch at that VA. Use list_patches to find addresses.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"address", {{"type","string"},{"description","VA of the patched byte"}}},
            }},
            {"required", nlohmann::json::array({"address"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        std::uint64_t va = 0; std::string e;
        if (!parseVa(args, "address", va, e)) { r.ok=false; r.error=e; return r; }
        const auto* fns = DbgFunctions();
        if (!fns || !fns->PatchRestore) { r.ok=false; r.error="DbgFunctions->PatchRestore is null"; return r; }
        const bool ok = fns->PatchRestore(static_cast<duint>(va));
        if (!ok) {
            r.ok=false; r.error="PatchRestore failed (no patch at " + formatHexU64(va) + "?)";
            return r;
        }
        XAI_LOG_INFO("restore_patch: va={}", formatHexU64(va).c_str());
        r.ok=true;
        r.data = {{"address", formatHexU64(va)}};
        return r;
    }
};

// ============= S7-H format_with_dbg =============
class FormatWithDbgTool : public ITool {
public:
    std::string name() const override { return "format_with_dbg"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "Render an x64dbg format template using current debuggee state. "
               "Supports expressions like '{rax}', '{x:[rsp+8]}', '{s:[rcx]}'. "
               "Output capped at 4 KB.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"template", {{"type","string"},{"description","x64dbg format string"}}},
            }},
            {"required", nlohmann::json::array({"template"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        if (!args.contains("template") || !args["template"].is_string()) {
            r.ok=false; r.error="'template' must be a string"; return r;
        }
        const std::string tmpl = args["template"].get<std::string>();
        const auto* fns = DbgFunctions();
        if (!fns || !fns->StringFormatInline) {
            r.ok=false; r.error="DbgFunctions->StringFormatInline is null"; return r;
        }
        std::vector<char> buf(kFormatBufSize, 0);
        const bool ok = fns->StringFormatInline(tmpl.c_str(), buf.size(), buf.data());
        if (!ok) {
            r.ok=false; r.error="StringFormatInline failed (invalid template?)";
            r.data = {{"template", tmpl}};
            return r;
        }
        std::string out(buf.data());
        r.ok=true;
        r.data = {
            {"template", tmpl},
            {"output",   std::move(out)},
        };
        return r;
    }
};

// ============= S7-I gui_focus_disasm =============
class GuiFocusDisasmTool : public ITool {
public:
    std::string name() const override { return "gui_focus_disasm"; }
    ToolCategory category() const override { return ToolCategory::DbgControl; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "Scroll the disassembly view to VA and highlight it. "
               "No effect on execution. Use to guide the user's attention.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"address", {{"type","string"},{"description","VA to focus, decimal or 0x hex"}}},
            }},
            {"required", nlohmann::json::array({"address"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        std::uint64_t va = 0; std::string e;
        if (!parseVa(args, "address", va, e)) { r.ok=false; r.error=e; return r; }
        GuiDisasmAt(static_cast<duint>(va), static_cast<duint>(va));
        r.ok=true;
        r.data = {{"address", formatHexU64(va)}};
        return r;
    }
};

// ============= S7-I gui_focus_dump =============
class GuiFocusDumpTool : public ITool {
public:
    std::string name() const override { return "gui_focus_dump"; }
    ToolCategory category() const override { return ToolCategory::DbgControl; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "Scroll the dump view to VA. Optional 'index' selects Dump 1..5. "
               "No effect on execution.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"address", {{"type","string"},{"description","VA to focus"}}},
                {"index",   {{"type","integer"},{"description","Dump pane 1..5; default 1"}}},
            }},
            {"required", nlohmann::json::array({"address"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        std::uint64_t va = 0; std::string e;
        if (!parseVa(args, "address", va, e)) { r.ok=false; r.error=e; return r; }
        int idx = 1;
        if (args.contains("index") && args["index"].is_number_integer()) {
            idx = args["index"].get<int>();
            if (idx < 1 || idx > 5) { r.ok=false; r.error="'index' must be in [1,5]"; return r; }
        }
        if (idx == 1) {
            GuiDumpAt(static_cast<duint>(va));
        } else {
            // GuiDumpAtN 用 0-based index
            GuiDumpAtN(static_cast<duint>(va), idx - 1);
        }
        r.ok=true;
        r.data = {{"address", formatHexU64(va)}, {"index", idx}};
        return r;
    }
};

void registerPatchMiscTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<ListPatchesTool>());
    reg.registerTool(std::make_unique<RestorePatchTool>());
    reg.registerTool(std::make_unique<FormatWithDbgTool>());
    reg.registerTool(std::make_unique<GuiFocusDisasmTool>());
    reg.registerTool(std::make_unique<GuiFocusDumpTool>());
}

}  // namespace x64ai
