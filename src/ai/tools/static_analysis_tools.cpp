// ai/tools/static_analysis_tools.cpp
//
// M4.4b 静态分析工具集（3 个）：
//   - find_xrefs_to       谁引用/调用了这个地址（caller 树关键）
//   - get_function_range  当前 va 所在函数的起止
//   - search_pattern      字节模式搜索（hex pattern，支持 ?? 通配）
//
// 注：x64dbg 静态信息（xref/function）依赖分析过的数据。
//   - xref：需要 x64dbg 已对该模块做过 "analyze module" 或在调试中触达过
//   - function range：DbgFunctionGet 返回当前函数定义，没分析则失败
//
// 所有工具不调试也能查（只要 x64dbg 进程在），但需要 DbgIsDebugging() 为 true
// 才有意义（否则 x64dbg 内部数据库可能为空）。

#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <Windows.h>

#include "bridgemain.h"
#include "_dbgfunctions.h"
#include "_scriptapi_pattern.h"

#include "util/logging.h"

namespace x64ai {

namespace {

bool parseUInt64(const nlohmann::json& v, std::uint64_t& out)
{
    if (v.is_number_unsigned()) { out = v.get<std::uint64_t>(); return true; }
    if (v.is_number_integer()) {
        auto i = v.get<std::int64_t>();
        if (i < 0) return false;
        out = static_cast<std::uint64_t>(i); return true;
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s.empty()) return false;
        try {
            std::size_t pos = 0;
            std::uint64_t r;
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                r = std::stoull(s.substr(2), &pos, 16);
                if (pos + 2 != s.size()) return false;
            } else {
                r = std::stoull(s, &pos, 0);
                if (pos != s.size()) return false;
            }
            out = r; return true;
        } catch (...) { return false; }
    }
    return false;
}

std::string formatHexVa(std::uint64_t va)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)va);
    return buf;
}

const char* xrefTypeName(XREFTYPE t)
{
    switch (t) {
    case XREF_DATA: return "data";
    case XREF_JMP:  return "jmp";
    case XREF_CALL: return "call";
    default:        return "none";
    }
}

// ====== find_xrefs_to ======

class FindXrefsToTool : public ITool {
public:
    std::string name() const override { return "find_xrefs_to"; }
    std::string description() const override
    {
        return "Find all addresses that reference (call/jmp/data-ref) the given virtual address. "
               "This is the reverse-direction caller lookup: given a function VA, returns the call sites. "
               "Each result includes the referencing VA, the reference type (call/jmp/data), "
               "and the module + nearby instruction for context.";
    }
    std::string descriptionZh() const override
    {
        return "查找所有引用指定 VA 的地址（call / jmp / 数据引用）。"
               "这是反向调用者查询：给定一个函数 VA，返回所有调用它的地点。"
               "每条结果包含引用方 VA、引用类型（call/jmp/data）、所在模块及附近指令上下文。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"va", {{"type", "string"}, {"description", "Target virtual address to find xrefs to."}}},
                {"max_results", {
                    {"type", "integer"},
                    {"description", "Maximum results returned (1-256). Default 64."},
                    {"minimum", 1},
                    {"maximum", 256},
                }},
            }},
            {"required", {"va"}},
        };
    }
    std::size_t maxResultBytes() const override { return 32 * 1024; }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active";
            return r;
        }
        std::uint64_t va = 0;
        if (!args.contains("va") || !parseUInt64(args["va"], va)) {
            r.ok = false; r.error = "invalid 'va'";
            return r;
        }
        int maxResults = 64;
        {
            std::string err;
            if (tryGetInt32Hint(args, "max_results", 1, 256, maxResults, err)) {
                // ok
            } else if (!err.empty()) {
                r.ok = false; r.error = "invalid 'max_results': " + err; return r;
            }
        }

        XREF_INFO info{};
        if (!DbgXrefGet(static_cast<duint>(va), &info)) {
            // 不视为错误：可能就是真的没有引用
            r.data = {
                {"target", formatHexVa(va)},
                {"count",  0},
                {"xrefs",  nlohmann::json::array()},
                {"note",   "no xrefs found (target may not be analyzed; run 'analyze module' in x64dbg)"},
            };
            return r;
        }
        std::unique_ptr<XREF_INFO, void(*)(XREF_INFO*)> guard(
            &info, [](XREF_INFO* p) { if (p->references) BridgeFree(p->references); });

        nlohmann::json arr = nlohmann::json::array();
        const int n = std::min<int>(static_cast<int>(info.refcount), maxResults);
        for (int i = 0; i < n; ++i) {
            const XREF_RECORD& rec = info.references[i];
            // 取所在模块名 + 反汇编一行帮 LLM 理解上下文
            char mod[MAX_MODULE_SIZE] = {0};
            DbgGetModuleAt(rec.addr, mod);
            BASIC_INSTRUCTION_INFO bi{};
            DbgDisasmFastAt(rec.addr, &bi);

            nlohmann::json item = {
                {"va",       formatHexVa(static_cast<std::uint64_t>(rec.addr))},
                {"type",     xrefTypeName(rec.type)},
                {"module",   mod[0] ? mod : ""},
                {"mnemonic", bi.instruction},
            };
            // 如果引用方所在函数已分析，附上 caller 函数起始
            duint fnStart = 0, fnEnd = 0;
            if (DbgFunctionGet(rec.addr, &fnStart, &fnEnd)) {
                item["caller_function"] = formatHexVa(static_cast<std::uint64_t>(fnStart));
            }
            arr.push_back(std::move(item));
        }

        r.data = {
            {"target",    formatHexVa(va)},
            {"count",     static_cast<int>(info.refcount)},
            {"returned",  n},
            {"truncated", info.refcount > static_cast<duint>(maxResults)},
            {"xrefs",     std::move(arr)},
        };
        return r;
    }
};

// ====== get_function_range ======

class GetFunctionRangeTool : public ITool {
public:
    std::string name() const override { return "get_function_range"; }
    std::string description() const override
    {
        return "Get the start and end addresses of the function containing the given VA. "
               "Useful before calling get_disasm on the full function body. "
               "Returns failure if x64dbg has not yet analyzed the function.";
    }
    std::string descriptionZh() const override
    {
        return "获取包含指定 VA 的函数的起止地址。在对整个函数体调用 get_disasm 前很有用。"
               "若 x64dbg 还未分析过该函数则返回失败。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"va", {{"type", "string"}, {"description", "Any VA inside the function."}}},
            }},
            {"required", {"va"}},
        };
    }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active";
            return r;
        }
        std::uint64_t va = 0;
        if (!args.contains("va") || !parseUInt64(args["va"], va)) {
            r.ok = false; r.error = "invalid 'va'";
            return r;
        }
        duint start = 0, end = 0;
        if (!DbgFunctionGet(static_cast<duint>(va), &start, &end)) {
            r.ok = false;
            r.error = "no function defined at " + formatHexVa(va) +
                      " (run 'analyze module' in x64dbg, or pass a VA inside an analyzed function)";
            return r;
        }
        char mod[MAX_MODULE_SIZE] = {0};
        DbgGetModuleAt(start, mod);
        r.data = {
            {"va",       formatHexVa(va)},
            {"start",    formatHexVa(static_cast<std::uint64_t>(start))},
            {"end",      formatHexVa(static_cast<std::uint64_t>(end))},
            {"size",     static_cast<std::uint64_t>(end - start + 1)},
            {"module",   mod[0] ? mod : ""},
        };
        return r;
    }
};

// ====== search_pattern ======

class SearchPatternTool : public ITool {
public:
    std::string name() const override { return "search_pattern"; }
    std::string description() const override
    {
        return "Search for a byte pattern in debuggee memory. "
               "Pattern uses x64dbg syntax: hex bytes separated by spaces, '?' for nibble wildcard, "
               "'??' for full-byte wildcard. Example: \"48 8B ?? 24 ?? E8\". "
               "Searches a single module range (specify 'module') or a custom range (specify 'start' + 'size'). "
               "Returns up to 'max_results' hit addresses.";
    }
    std::string descriptionZh() const override
    {
        return "在被调试进程内存中搜索字节模式。"
               "模式采用 x64dbg 语法：十六进制字节空格分隔，'?' 表示半字节通配，'??' 表示整字节通配。"
               "例如：\"48 8B ?? 24 ?? E8\"。"
               "可在单个模块内搜索（指定 'module'），或在自定义区间搜索（指定 'start' + 'size'）。"
               "最多返回 'max_results' 条命中地址。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"pattern", {{"type", "string"}, {"description", "x64dbg pattern, e.g. '48 8B ?? 24'."}}},
                {"module",  {{"type", "string"}, {"description", "Module name to search inside (preferred)."}}},
                {"start",   {{"type", "string"}, {"description", "Custom range start VA. Use with 'size'."}}},
                {"size",    {{"type", "string"}, {"description", "Custom range size in bytes."}}},
                {"max_results", {
                    {"type", "integer"},
                    {"description", "Max results (1-128). Default 32."},
                    {"minimum", 1},
                    {"maximum", 128},
                }},
            }},
            {"required", {"pattern"}},
        };
    }
    std::size_t maxResultBytes() const override { return 16 * 1024; }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active";
            return r;
        }
        if (!args.contains("pattern") || !args["pattern"].is_string()) {
            r.ok = false; r.error = "invalid 'pattern'";
            return r;
        }
        const std::string pattern = args["pattern"].get<std::string>();
        if (pattern.empty()) {
            r.ok = false; r.error = "empty pattern";
            return r;
        }
        int maxResults = 32;
        {
            std::string err;
            if (tryGetInt32Hint(args, "max_results", 1, 128, maxResults, err)) {
                // ok
            } else if (!err.empty()) {
                r.ok = false; r.error = "invalid 'max_results': " + err; return r;
            }
        }

        duint start = 0, size = 0;
        std::string scopeDesc;
        if (args.contains("module") && args["module"].is_string()) {
            const std::string mod = args["module"].get<std::string>();
            start = DbgModBaseFromName(mod.c_str());
            if (!start) {
                r.ok = false;
                r.error = "module not found: '" + mod + "' (try basename without extension, "
                          "or call list_modules first to see exact names)";
                return r;
            }
            // 取模块 size：用 DBGFUNCTIONS->ModSizeFromAddr(base)，比 Eval 'mod.size(...)' 稳。
            // 旧实现用 DbgEval("mod.size(\"name\")") 因为 x64dbg 表达式语法里函数参数不带引号，
            // 一律失败（见 known-issues K-29）。
            const auto* fns = DbgFunctions();
            if (fns && fns->ModSizeFromAddr) {
                size = fns->ModSizeFromAddr(start);
            }
            if (!size) {
                r.ok = false;
                r.error = "failed to get size of module '" + mod + "' (base=" + formatHexVa(static_cast<std::uint64_t>(start)) + ")";
                return r;
            }
            scopeDesc = "module " + mod;
        } else if (args.contains("start") && args.contains("size")) {
            std::uint64_t s = 0, n = 0;
            if (!parseUInt64(args["start"], s) || !parseUInt64(args["size"], n)) {
                r.ok = false; r.error = "invalid 'start' / 'size'";
                return r;
            }
            if (n == 0 || n > 256u * 1024u * 1024u) {
                r.ok = false; r.error = "'size' must be in (0, 256MB]";
                return r;
            }
            start = static_cast<duint>(s);
            size  = static_cast<duint>(n);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "range [0x%llX, +0x%llX]",
                          (unsigned long long)s, (unsigned long long)n);
            scopeDesc = buf;
        } else {
            r.ok = false;
            r.error = "must specify either 'module' or both 'start' and 'size'";
            return r;
        }

        // 循环找：每命中一个，从下一字节继续
        nlohmann::json hits = nlohmann::json::array();
        duint cur = start;
        duint remaining = size;
        for (int i = 0; i < maxResults && remaining > 0; ++i) {
            duint hit = Script::Pattern::FindMem(cur, remaining, pattern.c_str());
            if (!hit) break;
            if (hit < cur || hit >= start + size) break;  // 防御
            hits.push_back(formatHexVa(static_cast<std::uint64_t>(hit)));
            duint advance = (hit - cur) + 1;
            if (advance >= remaining) break;
            cur       += advance;
            remaining -= advance;
        }

        r.data = {
            {"pattern",  pattern},
            {"scope",    scopeDesc},
            {"count",    hits.size()},
            {"hits",     std::move(hits)},
        };
        return r;
    }
};

}  // namespace

void registerStaticAnalysisTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<FindXrefsToTool>(), "disasm-cfg");
    reg.registerTool(std::make_unique<GetFunctionRangeTool>(), "disasm-cfg");
    reg.registerTool(std::make_unique<SearchPatternTool>(), "memory-search");
}

}  // namespace x64ai
