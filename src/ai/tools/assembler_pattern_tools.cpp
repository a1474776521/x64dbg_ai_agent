// ai/tools/assembler_pattern_tools.cpp
//
// S7-C assemble_at   - 在指定 VA 汇编一条指令
// S7-D pattern_replace - 模式搜索并替换（??=通配）
// S7-F set_flag        - 设置 EFLAGS 单位
//
// 设计：
//   - assemble_at：默认 fill_nop=true（与 GUI 行为一致；用户可显式关闭）
//     · 内部用 AssembleMemEx 获取错误字符串；失败时把错误一并返回给 AI
//   - pattern_replace：Script::Pattern::SearchAndReplaceMem 已内置 ?? 通配，
//     不需要我们再做掩码计算；start+size 限定搜索范围（避免误扫整个进程）
//     · 上限 16 MB，与 set_page_protect 保持一致的安全线
//   - set_flag：name→FlagEnum 映射；非法 name 直接报错
#include "ai/tools/builtin_tools.h"
#include "ai/tools/dbg_state_util.h"  // K-43
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <Windows.h>
#include "bridgemain.h"
#include "_scriptapi_assembler.h"
#include "_scriptapi_pattern.h"
#include "_scriptapi_flag.h"

#include "util/logging.h"

namespace x64ai {

namespace {

constexpr std::uint64_t kPatternMaxSize = 16ull * 1024ull * 1024ull;

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

// ============= S7-C assemble_at =============
class AssembleAtTool : public ITool {
public:
    std::string name() const override { return "assemble_at"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Assemble one instruction at VA and write the bytes. "
               "If the new instruction is shorter than original, NOPs fill the gap "
               "by default (fill_nop=true). Returns assembled size on success.";
    }
    std::string descriptionZh() const override
    {
        return "在指定 VA 汇编一条指令并写入字节。"
               "若新指令比原指令短，默认（fill_nop=true）用 NOP 填充间隙。"
               "成功时返回汇编后的字节数。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"address",     {{"type","string"},{"description","VA, decimal or 0x hex"}}},
                {"instruction", {{"type","string"},{"description","Single asm line, e.g. 'mov eax, 1'"}}},
                {"fill_nop",    {{"type","boolean"},{"description","Pad trailing bytes with NOP; default true"}}},
            }},
            {"required", nlohmann::json::array({"address","instruction"})},
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
        if (!args.contains("instruction") || !args["instruction"].is_string()) {
            r.ok=false; r.error="'instruction' must be a string"; return r;
        }
        const std::string instr = args["instruction"].get<std::string>();
        if (instr.empty()) { r.ok=false; r.error="'instruction' is empty"; return r; }
        bool fillNop = true;
        if (args.contains("fill_nop") && args["fill_nop"].is_boolean()) {
            fillNop = args["fill_nop"].get<bool>();
        }

        int size = 0;
        char errbuf[MAX_ERROR_SIZE] = {0};
        const bool ok = Script::Assembler::AssembleMemEx(
            static_cast<duint>(va), instr.c_str(), &size, errbuf, fillNop);
        if (!ok) {
            r.ok=false;
            r.error = std::string("AssembleMemEx failed: ") +
                      (errbuf[0] ? errbuf : "unknown assembler error");
            r.data = {{"address", formatHexU64(va)}, {"instruction", instr}};
            return r;
        }
        XAI_LOG_INFO("assemble_at: va={} instr=\"{}\" size={} fill_nop={}",
                     formatHexU64(va).c_str(), instr.c_str(), size, fillNop);
        r.ok=true;
        r.data = {
            {"address", formatHexU64(va)},
            {"instruction", instr},
            {"size", size},
            {"fill_nop", fillNop},
        };
        return r;
    }
};

// ============= S7-D pattern_replace =============
class PatternReplaceTool : public ITool {
public:
    std::string name() const override { return "pattern_replace"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Search [start, start+size) for bytes matching search_pattern and replace with "
               "replace_pattern. Both use space-separated hex with '??' as wildcard, "
               "e.g. '74 ?? 8B' -> '90 ?? 8B'. Size capped at 16 MB.";
    }
    std::string descriptionZh() const override
    {
        return "在区间 [start, start+size) 搜索匹配 search_pattern 的字节并替换为 replace_pattern。"
               "两者均为空格分隔的十六进制串，'??' 为通配符，例如 '74 ?? 8B' -> '90 ?? 8B'。"
               "size 上限 16 MB。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"start",          {{"type","string"},{"description","Search start VA"}}},
                {"size",           {{"type","string"},{"description","Search range size in bytes (<=16MB)"}}},
                {"search_pattern", {{"type","string"},{"description","Bytes to match; '??' wildcard"}}},
                {"replace_pattern",{{"type","string"},{"description","Bytes to write; '??' keeps original"}}},
            }},
            {"required", nlohmann::json::array({"start","size","search_pattern","replace_pattern"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        std::uint64_t start = 0, size = 0; std::string e;
        if (!parseVa(args, "start", start, e)) { r.ok=false; r.error=e; return r; }
        if (!parseVa(args, "size",  size,  e)) { r.ok=false; r.error=e; return r; }
        if (size == 0) { r.ok=false; r.error="'size' must be > 0"; return r; }
        if (size > kPatternMaxSize) {
            r.ok=false;
            r.error="'size' exceeds 16MB safety cap";
            return r;
        }
        if (!args.contains("search_pattern") || !args["search_pattern"].is_string() ||
            !args.contains("replace_pattern") || !args["replace_pattern"].is_string()) {
            r.ok=false; r.error="'search_pattern' and 'replace_pattern' must be strings"; return r;
        }
        const std::string sp = args["search_pattern"].get<std::string>();
        const std::string rp = args["replace_pattern"].get<std::string>();
        if (sp.empty() || rp.empty()) { r.ok=false; r.error="patterns must be non-empty"; return r; }

        const bool ok = Script::Pattern::SearchAndReplaceMem(
            static_cast<duint>(start), static_cast<duint>(size), sp.c_str(), rp.c_str());
        if (!ok) {
            r.ok=false;
            r.error="SearchAndReplaceMem returned false (no match or invalid pattern)";
            r.data = {{"start", formatHexU64(start)},{"size", size},{"search", sp},{"replace", rp}};
            return r;
        }
        XAI_LOG_INFO("pattern_replace: start={} size={} \"{}\" -> \"{}\"",
                     formatHexU64(start).c_str(), static_cast<unsigned long long>(size),
                     sp.c_str(), rp.c_str());
        r.ok=true;
        r.data = {
            {"start",   formatHexU64(start)},
            {"size",    size},
            {"search",  sp},
            {"replace", rp},
        };
        return r;
    }
};

// ============= S7-F set_flag =============
class SetFlagTool : public ITool {
public:
    std::string name() const override { return "set_flag"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Set an EFLAGS bit on the currently focused thread. "
               "name ∈ {ZF,OF,CF,PF,SF,TF,AF,DF,IF}. Useful to force a branch.";
    }
    std::string descriptionZh() const override
    {
        return "在当前活动线程上设置一个 EFLAGS 标志位。"
               "name ∈ {ZF,OF,CF,PF,SF,TF,AF,DF,IF}。常用于强行影响分支跳转。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"name",  {{"type","string"},{"enum",
                            nlohmann::json::array({"ZF","OF","CF","PF","SF","TF","AF","DF","IF"})}}},
                {"value", {{"type","boolean"},{"description","true=1, false=0"}}},
            }},
            {"required", nlohmann::json::array({"name","value"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        // K-43: EFLAGS 同样存活在 thread context 里，要求 paused 才能改。
        // running 时 Script::Flag::Set 通常返回 false，且即便偶尔成功也会被立刻覆盖。
        if (DbgIsRunning()) {
            r.ok = false;
            r.error = "cannot set_flag: debuggee is currently 'running'; EFLAGS lives "
                      "in thread context and cannot be reliably written while the target "
                      "thread is executing. Call pause_debug first.";
            r.data = {{"current_state", "running"}};
            return r;
        }
        if (!args.contains("name") || !args["name"].is_string() ||
            !args.contains("value") || !args["value"].is_boolean()) {
            r.ok=false; r.error="'name'(string) and 'value'(bool) are required"; return r;
        }
        const std::string n = args["name"].get<std::string>();
        const bool v = args["value"].get<bool>();
        Script::Flag::FlagEnum f;
        if      (n == "ZF") f = Script::Flag::ZF;
        else if (n == "OF") f = Script::Flag::OF;
        else if (n == "CF") f = Script::Flag::CF;
        else if (n == "PF") f = Script::Flag::PF;
        else if (n == "SF") f = Script::Flag::SF;
        else if (n == "TF") f = Script::Flag::TF;
        else if (n == "AF") f = Script::Flag::AF;
        else if (n == "DF") f = Script::Flag::DF;
        else if (n == "IF") f = Script::Flag::IF;
        else { r.ok=false; r.error="invalid flag name '" + n + "'"; return r; }

        const bool ok = Script::Flag::Set(f, v);
        if (!ok) {
            r.ok = false;
            r.error = std::string("Script::Flag::Set returned false for '") + n
                      + "' (current_state=" + currentDbgStateStr() + ")";
            return r;
        }
        XAI_LOG_INFO("set_flag: {} = {}", n.c_str(), v ? 1 : 0);
        r.ok=true;
        r.data = {{"name", n}, {"value", v}};
        return r;
    }
};

void registerAssemblerPatternTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<AssembleAtTool>(), "write-patch");
    reg.registerTool(std::make_unique<PatternReplaceTool>(), "write-patch");
    reg.registerTool(std::make_unique<SetFlagTool>(), "write-patch");
}

}  // namespace x64ai
