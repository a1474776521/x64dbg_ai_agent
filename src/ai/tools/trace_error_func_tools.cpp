// ai/tools/trace_error_func_tools.cpp
//
// S8-E get_trace_record_info   - 查指定 VA 的 trace record（hit count + byte type + page record type）
// S8-E translate_error_code    - Win32/NT 错误码 → 符号名（如 0xC0000005 → ACCESS_VIOLATION）
// S8-E add_function            - 注册一个函数区间（修分析器漏识别）；含 manual=true 防覆盖
//
// 关键点：
//   GetTraceRecordHitCount / GetTraceRecordByteType 接受任意 VA，但只在该页已启用 trace record
//   时才有效；否则返回 0 / InstructionBody。
//   GetTraceRecordType / SetTraceRecordType 的参数必须是**页对齐地址**（pageAddress）。
//
//   translate_error_code 用 lazy-init 全局表：first call 调 EnumErrorCodes + EnumExceptions
//   拷贝到 unordered_map<duint, string>，后续 O(1)。注意 CONSTANTINFO.name 是 dbg 端字符串，
//   必须立即 std::string 拷贝。
//
//   Script::Function::Add(start, end, manual, instructionCount=0)：
//     - end 是**最后一条指令的起始地址**（inclusive），不是 end+1
//     - manual=true 让 x64dbg 不在自动分析时覆盖（用户/agent 手工标定的函数应该用 true）
//     - 已存在或重叠时返回 false
#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <Windows.h>
#include "bridgemain.h"
#include "bridgelist.h"
#include "_dbgfunctions.h"
#include "_scriptapi_function.h"

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

const char* byteTypeToString(TRACERECORDBYTETYPE t)
{
    switch (t) {
        case InstructionBody:        return "InstructionBody";
        case InstructionHeading:     return "InstructionHeading";
        case InstructionTailing:     return "InstructionTailing";
        case InstructionOverlapped:  return "InstructionOverlapped";
        case DataByte:               return "DataByte";
        case DataWord:               return "DataWord";
        case DataDWord:              return "DataDWord";
        case DataQWord:              return "DataQWord";
        case DataFloat:              return "DataFloat";
        case DataDouble:             return "DataDouble";
        case DataLongDouble:         return "DataLongDouble";
        case DataXMM:                return "DataXMM";
        case DataYMM:                return "DataYMM";
        case DataMMX:                return "DataMMX";
        case DataMixed:              return "DataMixed";
        case InstructionDataMixed:   return "InstructionDataMixed";
        default:                     return "Unknown";
    }
}

const char* recordTypeToString(TRACERECORDTYPE t)
{
    switch (t) {
        case TraceRecordNone:                          return "None";
        case TraceRecordBitExec:                       return "BitExec";
        case TraceRecordByteWithExecTypeAndCounter:    return "ByteWithExecTypeAndCounter";
        case TraceRecordWordWithExecTypeAndCounter:    return "WordWithExecTypeAndCounter";
        default:                                       return "Unknown";
    }
}

// ===== translate_error_code 的全局表（lazy init）=====
std::once_flag g_errCodeInitOnce;
std::unordered_map<std::uint64_t, std::string> g_errCodeMap;  // value → name
std::size_t g_errCodeCount = 0;

void ensureErrCodeTable()
{
    std::call_once(g_errCodeInitOnce, []{
        const auto* fns = DbgFunctions();
        if (!fns || !fns->EnumErrorCodes || !fns->EnumExceptions) {
            XAI_LOG_WARN("translate_error_code: EnumErrorCodes/EnumExceptions null");
            return;
        }
        auto fill = [](void(*api)(ListOf(CONSTANTINFO))) {
            BridgeList<CONSTANTINFO> list;
            api(&list);
            const int n = list.Count();
            for (int i = 0; i < n; ++i) {
                const auto& c = list[i];
                if (!c.name) continue;
                // value 可能与已有 key 冲突（Win32 errcode 与 NT exception 罕见但可能），
                // 后者覆盖（NT exception 通常更准确）。
                g_errCodeMap[static_cast<std::uint64_t>(c.value)] = std::string(c.name);
            }
        };
        fill(fns->EnumErrorCodes);
        fill(fns->EnumExceptions);
        g_errCodeCount = g_errCodeMap.size();
        XAI_LOG_INFO("translate_error_code: built table with {} entries", g_errCodeCount);
    });
}

}  // namespace

// ============= S8-E get_trace_record_info =============
class GetTraceRecordInfoTool : public ITool {
public:
    std::string name() const override { return "get_trace_record_info"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "Inspect trace record at VA: hit_count, byte_type "
               "(InstructionBody/Heading/Overlapped/...), and page record_type "
               "(None/BitExec/ByteWithExec.../WordWithExec...). hit_count=0 means "
               "either not executed or trace record not enabled for this page.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"address", {{"type","string"},{"description","VA to query"}}},
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
        if (!fns || !fns->GetTraceRecordHitCount || !fns->GetTraceRecordByteType
                 || !fns->GetTraceRecordType) {
            r.ok=false; r.error="DbgFunctions->GetTraceRecord* is null"; return r;
        }

        const unsigned int hits = fns->GetTraceRecordHitCount(static_cast<duint>(va));
        const TRACERECORDBYTETYPE bt = fns->GetTraceRecordByteType(static_cast<duint>(va));
        // page record type 要求页对齐地址
        const duint pageAddr = static_cast<duint>(va) & ~static_cast<duint>(0xFFF);
        const TRACERECORDTYPE rt = fns->GetTraceRecordType(pageAddr);

        r.ok=true;
        r.data = {
            {"address",         formatHexU64(va)},
            {"hit_count",       static_cast<unsigned>(hits)},
            {"byte_type",       byteTypeToString(bt)},
            {"page_address",    formatHexU64(static_cast<std::uint64_t>(pageAddr))},
            {"page_record_type", recordTypeToString(rt)},
        };
        if (rt == TraceRecordNone) {
            r.data["hint"] = "Trace record not enabled on this page; use x64dbg "
                             "'TraceRecord, enable' command or trace API to start recording.";
        }
        return r;
    }
};

// ============= S8-E translate_error_code =============
class TranslateErrorCodeTool : public ITool {
public:
    std::string name() const override { return "translate_error_code"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "Translate a Win32/NT error or exception code to its symbolic name "
               "(e.g. 0xC0000005 -> EXCEPTION_ACCESS_VIOLATION, 5 -> ERROR_ACCESS_DENIED). "
               "Returns ok=false if no match.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"code", {{"type","string"},{"description","Error/exception code, decimal or 0x hex"}}},
            }},
            {"required", nlohmann::json::array({"code"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        // 不需要 debuggerActive — 错误码翻译是纯查表操作（但表是从 DbgFunctions 取的，需 dbg 已加载）
        if (!DbgFunctions()) {
            r.ok=false; r.error="DbgFunctions is null"; return r;
        }
        std::uint64_t code = 0; std::string e;
        if (!parseVa(args, "code", code, e)) { r.ok=false; r.error=e; return r; }

        ensureErrCodeTable();
        auto it = g_errCodeMap.find(code);
        if (it == g_errCodeMap.end()) {
            // 32 位错误码常被写成负数，尝试低 32 位匹配
            const std::uint64_t low32 = code & 0xFFFFFFFFull;
            it = g_errCodeMap.find(low32);
        }
        if (it == g_errCodeMap.end()) {
            r.ok=false;
            r.error="no symbolic name found for code " + formatHexU64(code);
            r.data = {{"code", formatHexU64(code)},{"table_size", g_errCodeCount}};
            return r;
        }
        r.ok=true;
        r.data = {
            {"code",       formatHexU64(code)},
            {"name",       it->second},
            {"table_size", g_errCodeCount},
        };
        return r;
    }
};

// ============= S8-E add_function =============
class AddFunctionTool : public ITool {
public:
    std::string name() const override { return "add_function"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Register a function range that the analyzer missed. 'end' is the "
               "START address of the LAST instruction (inclusive), not end+1. "
               "manual=true (default) protects against re-analysis overwrite.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"start",  {{"type","string"},{"description","First instruction VA"}}},
                {"end",    {{"type","string"},{"description","LAST instruction VA (inclusive, not end+1)"}}},
                {"manual", {{"type","boolean"},{"description","Mark as user-defined; default true"}}},
            }},
            {"required", nlohmann::json::array({"start","end"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        std::uint64_t start = 0, end = 0; std::string e;
        if (!parseVa(args, "start", start, e)) { r.ok=false; r.error=e; return r; }
        if (!parseVa(args, "end",   end,   e)) { r.ok=false; r.error=e; return r; }
        if (end < start) {
            r.ok=false; r.error="'end' must be >= 'start'"; return r;
        }
        bool manual = true;
        if (args.contains("manual") && args["manual"].is_boolean()) {
            manual = args["manual"].get<bool>();
        }
        const bool ok = Script::Function::Add(
            static_cast<duint>(start), static_cast<duint>(end), manual);
        if (!ok) {
            r.ok=false;
            r.error="Function::Add failed (range overlaps existing function or invalid)";
            return r;
        }
        XAI_LOG_INFO("add_function: [{}, {}] manual={}",
                     formatHexU64(start).c_str(), formatHexU64(end).c_str(), manual);
        r.ok=true;
        r.data = {
            {"start",  formatHexU64(start)},
            {"end",    formatHexU64(end)},
            {"manual", manual},
        };
        return r;
    }
};

void registerTraceErrorFuncTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<GetTraceRecordInfoTool>());
    reg.registerTool(std::make_unique<TranslateErrorCodeTool>());
    reg.registerTool(std::make_unique<AddFunctionTool>());
}

}  // namespace x64ai
