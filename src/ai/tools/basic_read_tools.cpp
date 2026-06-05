// ai/tools/basic_read_tools.cpp
//
// M4.4a 基础读取工具集（纯只读）：
//   - get_disasm        反汇编指定地址附近 N 行
//   - read_memory       读裸内存（hex+ascii dump）
//   - read_string       读 C 字符串（自动判 ANSI / UTF-16）
//   - get_registers     当前线程寄存器快照
//   - list_modules      已加载模块列表
//   - eval_expression   (S1, T-11) 求值 x64dbg 表达式
//   - list_breakpoints  (S1, T-12) 列出所有断点
//
// 所有工具：
//   - DbgIsDebugging()==false 时直接返回 ok=false
//   - 地址 / 大小做严格校验，不允许参数把进程拖垮
//   - 不写内存、不改寄存器、不动断点
#include "ai/tools/builtin_tools.h"
#include "ai/tools/dbg_state_util.h"  // K-43: stale 标记
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
#include "bridgelist.h"
#include "_scriptapi_module.h"

#include "util/logging.h"

namespace x64ai {

namespace {

// ====== 通用工具函数 ======

bool parseUInt64(const nlohmann::json& v, std::uint64_t& out)
{
    if (v.is_number_unsigned()) {
        out = v.get<std::uint64_t>();
        return true;
    }
    if (v.is_number_integer()) {
        auto i = v.get<std::int64_t>();
        if (i < 0) return false;
        out = static_cast<std::uint64_t>(i);
        return true;
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
            out = r;
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

// S0-H1 / S2-E：parseInt32Lenient 已迁到 ai/tools/tool_args_util.h（共享给所有工具）。

std::string formatHexVa(std::uint64_t va)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)va);
    return buf;
}

// hex dump：返回 "00 11 22 ..  |abc.|" 多行字符串（每行 16 字节）
std::string hexDump(std::uint64_t baseVa, const unsigned char* p, std::size_t n)
{
    std::string out;
    out.reserve(n * 5);
    char line[128];
    for (std::size_t off = 0; off < n; off += 16) {
        const std::size_t row = std::min<std::size_t>(16, n - off);
        int w = std::snprintf(line, sizeof(line), "0x%016llX  ",
                              (unsigned long long)(baseVa + off));
        for (std::size_t i = 0; i < row; ++i) {
            w += std::snprintf(line + w, sizeof(line) - w, "%02X ", p[off + i]);
        }
        for (std::size_t i = row; i < 16; ++i) {
            w += std::snprintf(line + w, sizeof(line) - w, "   ");
        }
        w += std::snprintf(line + w, sizeof(line) - w, " |");
        for (std::size_t i = 0; i < row; ++i) {
            unsigned char c = p[off + i];
            line[w++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        line[w++] = '|';
        line[w] = '\0';
        out.append(line);
        out.push_back('\n');
    }
    return out;
}

// ====== get_disasm ======

class GetDisasmTool : public ITool {
public:
    std::string name() const override { return "get_disasm"; }
    std::string description() const override
    {
        return "Disassemble N instructions starting at the given virtual address. "
               "Returns each line with VA, raw bytes, and mnemonic. "
               "Use this to inspect any code region (function body, sub-call target, jump destination).";
    }
    std::string descriptionZh() const override
    {
        return "从指定虚拟地址开始反汇编 N 条指令。每行返回 VA、原始字节和助记符。"
               "用于查看任意代码区域（函数体、子调用目标、跳转目的地等）。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"va", {
                    {"type", "string"},
                    {"description", "Virtual address. Hex string like \"0x4012a0\" or decimal."},
                }},
                {"lines", {
                    {"type", "integer"},
                    {"description", "Number of instructions to disassemble (1-512). Default 64."},
                    {"minimum", 1},
                    {"maximum", 512},
                }},
            }},
            {"required", {"va"}},
        };
    }
    // 反汇编输出可能较大：默认 64 行 ~ 5KB；放宽到 32KB
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
        int lines = 64;
        if (args.contains("lines")) {
            std::string err;
            if (!parseInt32Lenient(args["lines"], 1, 512, lines, err)) {
                r.ok = false; r.error = "'lines': " + err;
                return r;
            }
        }

        nlohmann::json arr = nlohmann::json::array();
        duint cur = static_cast<duint>(va);
        for (int i = 0; i < lines; ++i) {
            BASIC_INSTRUCTION_INFO info{};
            DbgDisasmFastAt(cur, &info);
            if (info.size <= 0 || info.size > 16) break;

            unsigned char raw[16] = {0};
            DbgMemRead(cur, raw, static_cast<duint>(info.size));
            char hex[64] = {0};
            int hw = 0;
            for (int b = 0; b < info.size && hw + 3 < (int)sizeof(hex); ++b) {
                hw += std::snprintf(hex + hw, sizeof(hex) - hw,
                                    b ? " %02X" : "%02X", raw[b]);
            }

            nlohmann::json item = {
                {"va",        formatHexVa(static_cast<std::uint64_t>(cur))},
                {"size",      info.size},
                {"bytes",     hex},
                {"mnemonic",  info.instruction},
            };
            if (info.branch) {
                item["branch"] = info.call ? "call" : "jmp";
                if (info.addr) item["target"] = formatHexVa(static_cast<std::uint64_t>(info.addr));
            }
            arr.push_back(std::move(item));
            cur += info.size;
        }

        r.data = {
            {"start", formatHexVa(va)},
            {"end",   formatHexVa(static_cast<std::uint64_t>(cur))},
            {"count", arr.size()},
            {"instructions", std::move(arr)},
        };
        return r;
    }
};

// ====== read_memory ======

class ReadMemoryTool : public ITool {
public:
    std::string name() const override { return "read_memory"; }
    std::string description() const override
    {
        return "Read raw bytes from the debuggee at the given virtual address. "
               "Returns a hex+ASCII dump (16 bytes per row). "
               "Use this to inspect data structures, strings (prefer read_string), or pointed-to buffers.";
    }
    std::string descriptionZh() const override
    {
        return "从被调试进程的指定虚拟地址读取原始字节。返回 hex+ASCII 双栏 dump（每行 16 字节）。"
               "用于检查数据结构、字符串（字符串建议优先用 read_string）或指针指向的缓冲区。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"va",   {{"type", "string"}, {"description", "Virtual address."}}},
                {"size", {
                    {"type", "integer"},
                    {"description", "Number of bytes to read (1-65536, hard cap)."},
                    {"minimum", 1},
                    {"maximum", 65536},
                }},
            }},
            {"required", {"va", "size"}},
        };
    }
    std::size_t maxResultBytes() const override { return 96 * 1024; }  // 64KB raw + dump overhead

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
        if (!args.contains("size")) {
            r.ok = false; r.error = "'size' is required";
            return r;
        }
        int size = 0;
        {
            std::string err;
            if (!parseInt32Lenient(args["size"], 1, 65536, size, err)) {
                r.ok = false; r.error = "'size': " + err;
                return r;
            }
        }

        std::unique_ptr<unsigned char[]> buf(new unsigned char[size]);
        std::memset(buf.get(), 0, size);
        if (!DbgMemRead(static_cast<duint>(va), buf.get(), static_cast<duint>(size))) {
            r.ok = false;
            r.error = "DbgMemRead failed at " + formatHexVa(va) +
                      " (size=" + std::to_string(size) + "); memory unreadable?";
            return r;
        }

        r.data = {
            {"va",   formatHexVa(va)},
            {"size", size},
            {"dump", hexDump(va, buf.get(), size)},
        };
        return r;
    }
};

// ====== read_string ======

class ReadStringTool : public ITool {
public:
    std::string name() const override { return "read_string"; }
    std::string description() const override
    {
        return "Read a NUL-terminated C string at the given virtual address. "
               "Auto-detects ANSI (UTF-8) or UTF-16LE based on x64dbg's heuristic. "
               "Use this when a pointer / immediate looks like a string reference.";
    }
    std::string descriptionZh() const override
    {
        return "读取指定虚拟地址处的 C 风格 NUL 结尾字符串。"
               "依 x64dbg 的启发式自动判断 ANSI（UTF-8）或 UTF-16LE。"
               "当某个指针/立即数看起来像字符串引用时用此工具。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"va", {{"type", "string"}, {"description", "Virtual address."}}},
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

        // DbgGetStringAt 内部会判 ANSI/Unicode 并加 "A:..." / "U:..." 前缀（取决于版本）
        char buf[MAX_STRING_SIZE] = {0};
        if (!DbgGetStringAt(static_cast<duint>(va), buf)) {
            r.ok = false;
            r.error = "no string at " + formatHexVa(va);
            return r;
        }
        r.data = {
            {"va",   formatHexVa(va)},
            {"text", buf},
        };
        return r;
    }
};

// ====== get_registers ======

class GetRegistersTool : public ITool {
public:
    std::string name() const override { return "get_registers"; }
    std::string description() const override
    {
        return "Get the current thread's general-purpose registers (CPU + flags + segment). "
               "Use this to inspect program state at a breakpoint, check call arguments, "
               "or examine the result of an instruction.";
    }
    std::string descriptionZh() const override
    {
        return "获取当前线程的通用寄存器（CPU 寄存器 + 标志位 + 段寄存器）快照。"
               "用于在断点处检查程序状态、查看调用参数、或确认某条指令的执行结果。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
        };
    }

    ToolResult invoke(const nlohmann::json& /*args*/, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active";
            return r;
        }
        REGDUMP_AVX512 rd{};
        if (!DbgGetRegDumpEx(&rd, sizeof(rd))) {
            r.ok = false; r.error = "DbgGetRegDumpEx failed";
            return r;
        }

        auto fmt = [](std::uint64_t v) { return formatHexVa(v); };

        const auto& ctxr = rd.regcontext;
        nlohmann::json gpr = {
#ifdef _WIN64
            {"rax", fmt(ctxr.cax)}, {"rbx", fmt(ctxr.cbx)},
            {"rcx", fmt(ctxr.ccx)}, {"rdx", fmt(ctxr.cdx)},
            {"rsi", fmt(ctxr.csi)}, {"rdi", fmt(ctxr.cdi)},
            {"rsp", fmt(ctxr.csp)}, {"rbp", fmt(ctxr.cbp)},
            {"rip", fmt(ctxr.cip)},
            {"r8",  fmt(ctxr.r8)},  {"r9",  fmt(ctxr.r9)},
            {"r10", fmt(ctxr.r10)}, {"r11", fmt(ctxr.r11)},
            {"r12", fmt(ctxr.r12)}, {"r13", fmt(ctxr.r13)},
            {"r14", fmt(ctxr.r14)}, {"r15", fmt(ctxr.r15)},
#else
            {"eax", fmt(ctxr.cax)}, {"ebx", fmt(ctxr.cbx)},
            {"ecx", fmt(ctxr.ccx)}, {"edx", fmt(ctxr.cdx)},
            {"esi", fmt(ctxr.csi)}, {"edi", fmt(ctxr.cdi)},
            {"esp", fmt(ctxr.csp)}, {"ebp", fmt(ctxr.cbp)},
            {"eip", fmt(ctxr.cip)},
#endif
            {"eflags", fmt(ctxr.eflags)},
        };

        r.data = {
            {"gpr",       std::move(gpr)},
            {"lastError", fmt(static_cast<std::uint64_t>(rd.lastError))},
        };
        // K-43: running 时 DbgGetRegDumpEx 返回的是 debugger 缓存的上次 paused 快照——
        // 不是实时寄存器。LLM 拿 rax 做下一步运算（如 va = rax + N）会全错。
        // 不拒绝（保留 debug 价值），但 stale=true 让 LLM 知道别当真。
        const bool stale = DbgIsRunning();
        r.data["current_state"] = currentDbgStateStr();
        r.data["stale"]         = stale;
        if (stale) {
            r.data["stale_note"] =
                "debuggee is currently running; register values reflect the last paused "
                "snapshot, not live thread state. Call pause_debug + get_registers again "
                "if you need accurate values.";
        }
        return r;
    }
};

// ====== list_modules ======

class ListModulesTool : public ITool {
public:
    std::string name() const override { return "list_modules"; }
    std::string description() const override
    {
        return "List all modules currently loaded by the debuggee. "
               "Returns name, base, size, and entry point for each. "
               "Use this to find the main module / DLL of interest before drilling down.";
    }
    std::string descriptionZh() const override
    {
        return "列出被调试进程当前加载的所有模块。"
               "每条返回模块名、基址、大小、入口点。"
               "通常用于在进一步分析前先定位主模块或目标 DLL。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
        };
    }
    std::size_t maxResultBytes() const override { return 64 * 1024; }

    ToolResult invoke(const nlohmann::json& /*args*/, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active";
            return r;
        }
        ListInfo li{};
        if (!Script::Module::GetList(&li) || !li.data || li.count <= 0) {
            r.ok = false; r.error = "Script::Module::GetList failed";
            return r;
        }
        auto* arr = reinterpret_cast<Script::Module::ModuleInfo*>(li.data);
        nlohmann::json out = nlohmann::json::array();
        out.get_ptr<nlohmann::json::array_t*>()->reserve(li.count);
        for (int i = 0; i < li.count; ++i) {
            const auto& m = arr[i];
            out.push_back({
                {"name",  m.name},
                {"base",  formatHexVa(static_cast<std::uint64_t>(m.base))},
                {"size",  formatHexVa(static_cast<std::uint64_t>(m.size))},
                {"entry", formatHexVa(static_cast<std::uint64_t>(m.entry))},
                {"path",  m.path},
            });
        }
        BridgeFree(li.data);

        r.data = {
            {"count",   li.count},
            {"modules", std::move(out)},
        };
        return r;
    }
};

// ====== eval_expression (S1, T-11) ======
//
// 让 LLM 把 x64dbg 表达式（"[rbp+8]+10"、"GetProcAddress"、"401000+30" 等）交给
// 原生求值器，避免它自己做不靠谱的整数运算。
//
// 返回字段：
//   value      —— 0x 前缀十六进制
//   value_dec  —— 十进制（便于 LLM 直接判定大小 / 是否合理）
//   expr       —— 回显输入
class EvalExpressionTool : public ITool {
public:
    std::string name() const override { return "eval_expression"; }
    std::string description() const override
    {
        return "Evaluate an x64dbg expression in the debuggee's context. "
               "Accepts arithmetic (+ - * /), dereference '[expr]', registers (rax/eip), "
               "module-relative symbols (kernel32.GetProcAddress), and hex/decimal literals. "
               "Use this whenever you need to compute an address or read a small typed value "
               "instead of doing math yourself.";
    }
    std::string descriptionZh() const override
    {
        return "在被调试进程上下文中求值一个 x64dbg 表达式。"
               "支持算术运算（+ - * /）、解引用 [expr]、寄存器（rax/eip 等）、"
               "模块相对符号（如 kernel32.GetProcAddress）以及十六进制/十进制字面量。"
               "需要计算地址或读取小段类型化数据时优先用本工具，避免自己手算。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"expr", {
                    {"type", "string"},
                    {"description", "x64dbg expression, e.g. '[rbp+8]', 'kernel32.GetProcAddress', '401000+30'"},
                }},
            }},
            {"required", nlohmann::json::array({"expr"})},
        };
    }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active";
            return r;
        }
        if (!args.contains("expr") || !args["expr"].is_string()) {
            r.ok = false; r.error = "'expr' is required (string)";
            return r;
        }
        const std::string expr = args["expr"].get<std::string>();
        if (expr.empty() || expr.size() > 512) {
            r.ok = false; r.error = "'expr' length out of range [1, 512]";
            return r;
        }

        bool  ok    = false;
        duint value = DbgEval(expr.c_str(), &ok);
        if (!ok) {
            r.ok    = false;
            r.error = "expression evaluation failed: " + expr;
            return r;
        }

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%llu",
                      static_cast<unsigned long long>(value));
        // K-43: 表达式可能含寄存器（rax/cip）或栈解引用（[rbp+8]）等动态量；running 时
        // 这些都基于 stale 快照求值。静态量（mod.base()、imagebase()、字面量）不受影响。
        // 不拒绝，但回报 current_state 让 LLM 自行判断要不要先 pause_debug。
        r.data = {
            {"expr",          expr},
            {"value",         formatHexVa(static_cast<std::uint64_t>(value))},
            {"value_dec",     buf},
            {"current_state", currentDbgStateStr()},
        };
        if (DbgIsRunning()) {
            r.data["stale_note"] =
                "debuggee is running; if the expression dereferences registers or memory "
                "that may have changed (e.g. [rsp], rax, cip), the result may be stale.";
        }
        return r;
    }
};

// ====== list_breakpoints (S1, T-12) ======
//
// 列出所有当前安装的断点（normal / hardware / memory / dll / exception）。
// 每条返回 type / addr / enabled / active / singleshoot / hitCount / mod / name。
// "active" 在 x64dbg 中表示断点字节当前是否真正生效（被 step-over 临时禁用时 false）。
//
// 输出条数硬上限 1024，超出截断（防止极端用户开几万条 trace 断点把 LLM 灌穿）。
class ListBreakpointsTool : public ITool {
public:
    std::string name() const override { return "list_breakpoints"; }
    std::string description() const override
    {
        return "List all installed breakpoints (software, hardware, memory, DLL, exception). "
               "Returns address, type, enabled/active state, hit count, module and name. "
               "Use this to confirm a breakpoint was set, audit existing breakpoints, "
               "or pick one to delete.";
    }
    std::string descriptionZh() const override
    {
        return "列出当前所有已安装的断点（软件 / 硬件 / 内存 / DLL / 异常）。"
               "每条返回地址、类型、启用/激活状态、命中次数、所在模块与名称。"
               "用于确认断点已设置、审计现有断点、或挑选要删除的断点。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"type", {
                    {"type", "string"},
                    {"description",
                     "Filter by type: 'all' (default) | 'software' | 'hardware' | 'memory' | 'dll' | 'exception'"},
                }},
            }},
        };
    }
    std::size_t maxResultBytes() const override { return 64 * 1024; }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active";
            return r;
        }

        std::string filter = "all";
        if (args.contains("type") && args["type"].is_string()) {
            filter = args["type"].get<std::string>();
        }

        struct Cat { const char* name; BPXTYPE type; };
        const Cat all[] = {
            {"software",  bp_normal},
            {"hardware",  bp_hardware},
            {"memory",    bp_memory},
            {"dll",       bp_dll},
            {"exception", bp_exception},
        };

        nlohmann::json items = nlohmann::json::array();
        int totalEmitted = 0;
        bool truncated  = false;
        const int kHardCap = 1024;

        for (const auto& c : all) {
            if (filter != "all" && filter != c.name) continue;

            BPMAP bplist{};
            if (!DbgGetBpList(c.type, &bplist) || !bplist.bp || bplist.count <= 0) {
                if (bplist.bp) BridgeFree(bplist.bp);
                continue;
            }
            for (int i = 0; i < bplist.count && !truncated; ++i) {
                const auto& bp = bplist.bp[i];
                items.push_back({
                    {"type",        c.name},
                    {"addr",        formatHexVa(static_cast<std::uint64_t>(bp.addr))},
                    {"enabled",     bp.enabled},
                    {"active",      bp.active},
                    {"singleshoot", bp.singleshoot},
                    {"hitCount",    bp.hitCount},
                    {"mod",         bp.mod},
                    {"name",        bp.name},
                });
                if (++totalEmitted >= kHardCap) {
                    truncated = true;
                }
            }
            BridgeFree(bplist.bp);
            if (truncated) break;
        }

        r.data = {
            {"count",       totalEmitted},
            {"truncated",   truncated},
            {"breakpoints", std::move(items)},
        };
        return r;
    }
};

}  // namespace

void registerBasicReadTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<GetDisasmTool>(), "disasm-cfg");
    reg.registerTool(std::make_unique<ReadMemoryTool>(), "memory-search");
    reg.registerTool(std::make_unique<ReadStringTool>(), "memory-search");
    reg.registerTool(std::make_unique<GetRegistersTool>(), "register-stack");
    reg.registerTool(std::make_unique<ListModulesTool>(), "static-info");
    reg.registerTool(std::make_unique<EvalExpressionTool>(), "register-stack");
    reg.registerTool(std::make_unique<ListBreakpointsTool>(), "breakpoint");
}

}  // namespace x64ai
