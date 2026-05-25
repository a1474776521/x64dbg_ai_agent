// ai/tools/program_map_tools.cpp
//
// S6-D/E：程序结构 + 内存映射（"程序地图"类工具）。
//
// S6-D 内存映射：
//   get_memory_map         DbgMemMap → MEMPAGE 列表（基址/大小/state/protect/info）
//   get_page_protect       Script::Memory::GetProtect → PAGE_* 标志位
//   set_page_protect       Script::Memory::SetProtect → 改 RWX（Write 类，5s confirm）
//
// S6-E 程序地图：
//   list_functions         Script::Function::GetList → 所有已分析函数
//   get_module_imports     Script::Module::GetImports → IAT
//   get_module_exports     Script::Module::GetExports → EAT
//
// 共性：
//   - 6 个工具里只有 set_page_protect 是 Write；其它 5 个全是 Read。
//   - list_functions / imports / exports 单次返回可能很大；maxResultBytes 调到 256 KB。
//     超出由 ToolRegistry 截断。LLM 应优先按模块过滤。
//   - get_memory_map 返回的 protect 字段用人类可读字符串（RWX / RW- / R-- 等），
//     而不是 PAGE_* 原始数值，方便 LLM 理解。
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
#include "_scriptapi_memory.h"
#include "_scriptapi_module.h"
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

// PAGE_* → "RWX" 人类可读字符串
std::string protectToString(DWORD p)
{
    if (p == 0) return "---";
    const DWORD base = p & 0xFF;  // 屏蔽 PAGE_GUARD / NOCACHE / WRITECOMBINE 修饰位
    std::string s;
    switch (base) {
        case PAGE_NOACCESS:          s = "---"; break;
        case PAGE_READONLY:          s = "R--"; break;
        case PAGE_READWRITE:         s = "RW-"; break;
        case PAGE_WRITECOPY:         s = "RWC"; break;  // copy-on-write
        case PAGE_EXECUTE:           s = "--X"; break;
        case PAGE_EXECUTE_READ:      s = "R-X"; break;
        case PAGE_EXECUTE_READWRITE: s = "RWX"; break;
        case PAGE_EXECUTE_WRITECOPY: s = "RXC"; break;
        default:                     s = "???"; break;
    }
    if (p & PAGE_GUARD)         s += "+G";
    if (p & PAGE_NOCACHE)       s += "+N";
    if (p & PAGE_WRITECOMBINE)  s += "+W";
    return s;
}

// "RWX" 字符串 → PAGE_* 数值（不支持修饰位）
bool protectFromString(const std::string& in, DWORD& out, std::string& err)
{
    std::string s; s.reserve(in.size());
    for (char c : in) if (c != '-' && c != ' ') s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    // s ∈ {"", "R", "W", "X", "RW", "RX", "RWX", ...}
    const bool r = s.find('R') != std::string::npos;
    const bool w = s.find('W') != std::string::npos;
    const bool x = s.find('X') != std::string::npos;
    if (!r && !w && !x) { out = PAGE_NOACCESS; return true; }
    if (x &&  w && r)   { out = PAGE_EXECUTE_READWRITE; return true; }
    if (x &&  r && !w)  { out = PAGE_EXECUTE_READ;      return true; }
    if (x && !r && !w)  { out = PAGE_EXECUTE;           return true; }
    if (!x && w && r)   { out = PAGE_READWRITE;         return true; }
    if (!x && r && !w)  { out = PAGE_READONLY;          return true; }
    err = "unsupported protect string: '" + in + "' (use R/W/X combos like 'RW' / 'RWX' / 'R-X')";
    return false;
}

}  // namespace

// ============= S6-D get_memory_map =============
class GetMemoryMapTool : public ITool {
public:
    std::string name() const override { return "get_memory_map"; }
    std::string description() const override
    {
        return "Get the debuggee's full virtual memory map. Returns base/size/state/protect "
               "(RWX string) and info (module/section name) per page. Use to locate executable "
               "regions, RWX pages (unpacker artifact), heap/stack ranges.";
    }
    std::string descriptionZh() const override
    {
        return "获取被调试进程完整的虚拟内存映射表。每页返回 base/size/state/protect（RWX 字符串）"
               "和 info（模块/节名）。用于定位可执行区、RWX 页（脱壳痕迹）、堆栈区间。";
    }
    nlohmann::json parametersSchema() const override
    {
        return { {"type","object"}, {"properties", nlohmann::json::object()} };
    }
    std::size_t maxResultBytes() const override { return 256 * 1024; }
    ToolResult invoke(const nlohmann::json& /*args*/, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        MEMMAP mm{};
        if (!DbgMemMap(&mm) || mm.count <= 0 || !mm.page) {
            r.ok = false; r.error = "DbgMemMap failed"; return r;
        }
        nlohmann::json arr = nlohmann::json::array();
        for (int i = 0; i < mm.count; ++i) {
            const MEMPAGE& mp = mm.page[i];
            const auto& mbi = mp.mbi;
            const char* stateStr =
                (mbi.State == MEM_COMMIT)  ? "commit" :
                (mbi.State == MEM_RESERVE) ? "reserve" :
                (mbi.State == MEM_FREE)    ? "free"   : "unknown";
            const char* typeStr =
                (mbi.Type == MEM_IMAGE)   ? "image"   :
                (mbi.Type == MEM_MAPPED)  ? "mapped"  :
                (mbi.Type == MEM_PRIVATE) ? "private" : "unknown";
            arr.push_back({
                {"base",    formatHexU64(reinterpret_cast<std::uint64_t>(mbi.BaseAddress))},
                {"size",    formatHexU64(static_cast<std::uint64_t>(mbi.RegionSize))},
                {"state",   stateStr},
                {"type",    typeStr},
                {"protect", protectToString(mbi.Protect)},
                {"info",    mp.info},
            });
        }
        BridgeFree(mm.page);  // SDK 约定：调用方释放
        r.ok = true;
        r.data = {{"count", static_cast<int>(arr.size())}, {"pages", std::move(arr)}};
        return r;
    }
};

// ============= S6-D get_page_protect =============
class GetPageProtectTool : public ITool {
public:
    std::string name() const override { return "get_page_protect"; }
    std::string description() const override
    {
        return "Get the memory protection at an address. Returns RWX-style string and the "
               "containing page base/size.";
    }
    std::string descriptionZh() const override
    {
        return "获取指定地址的内存保护属性。返回 RWX 风格字符串及其所属页的 base/size。";
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
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        std::uint64_t va = 0; std::string e;
        if (!parseVa(args, "address", va, e)) { r.ok=false; r.error=e; return r; }
        const DWORD p   = Script::Memory::GetProtect(static_cast<duint>(va));
        const duint base = Script::Memory::GetBase(static_cast<duint>(va));
        const duint sz   = Script::Memory::GetSize(static_cast<duint>(va));
        r.ok = true;
        r.data = {
            {"address", formatHexU64(va)},
            {"protect", protectToString(p)},
            {"protect_raw", static_cast<std::uint32_t>(p)},
            {"page_base", formatHexU64(static_cast<std::uint64_t>(base))},
            {"page_size", formatHexU64(static_cast<std::uint64_t>(sz))},
        };
        return r;
    }
};

// ============= S6-D set_page_protect =============
class SetPageProtectTool : public ITool {
public:
    std::string name() const override { return "set_page_protect"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Change memory protection for a region (calls VirtualProtectEx). "
               "protect is RWX-style ('RW', 'RWX', 'R-X', '---' etc.). "
               "size is in bytes; affects all pages covering [address, address+size).";
    }
    std::string descriptionZh() const override
    {
        return "修改一段内存的保护属性（底层调用 VirtualProtectEx）。"
               "protect 为 RWX 风格字符串（如 'RW'、'RWX'、'R-X'、'---'）。"
               "size 单位为字节；影响覆盖 [address, address+size) 的所有页。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"address", {{"type","string"},{"description","VA, decimal or 0x hex"}}},
                {"protect", {{"type","string"},{"description","RWX-style string e.g. 'RWX'"}}},
                {"size",    {{"type","integer"},{"description","Bytes; 1 ≤ size ≤ 16 MB"}}},
            }},
            {"required", nlohmann::json::array({"address","protect","size"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        std::uint64_t va = 0; std::string e;
        if (!parseVa(args, "address", va, e)) { r.ok=false; r.error=e; return r; }
        if (!args.contains("protect") || !args["protect"].is_string()) {
            r.ok=false; r.error="'protect' required (string e.g. 'RWX')"; return r;
        }
        DWORD newProt = 0;
        if (!protectFromString(args["protect"].get<std::string>(), newProt, e)) {
            r.ok=false; r.error=e; return r;
        }
        int sz = 0;
        if (!tryGetInt32Hint(args, "size", 1, 16 * 1024 * 1024, sz, e)) {
            r.ok=false; r.error="invalid 'size': "+e; return r;
        }
        const bool ok = Script::Memory::SetProtect(static_cast<duint>(va),
                                                   static_cast<unsigned int>(newProt),
                                                   static_cast<duint>(sz));
        if (!ok) {
            r.ok=false; r.error = "Script::Memory::SetProtect failed at " + formatHexU64(va);
            return r;
        }
        XAI_LOG_INFO("set_page_protect: va={} size={} -> {}",
                     formatHexU64(va).c_str(), sz, protectToString(newProt).c_str());
        r.ok = true;
        r.data = {
            {"address", formatHexU64(va)},
            {"size",    sz},
            {"protect", protectToString(newProt)},
        };
        return r;
    }
};

// ============= S6-E list_functions =============
class ListFunctionsTool : public ITool {
public:
    std::string name() const override { return "list_functions"; }
    std::string description() const override
    {
        return "List all functions discovered by x64dbg's analyzer (module + rva range + manual flag + "
               "instruction count). Use to get a 'program map'; cross-reference with list_labels for names.";
    }
    std::string descriptionZh() const override
    {
        return "列出 x64dbg 分析器识别出的全部函数（模块 + RVA 区间 + 是否手动标注 + 指令数）。"
               "用于构建程序整体地图；可与 list_labels 交叉对照得到函数名。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"module", {{"type","string"},
                            {"description","Optional case-insensitive substring filter on module name"}}},
            }},
        };
    }
    std::size_t maxResultBytes() const override { return 256 * 1024; }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        std::string filter;
        if (args.contains("module") && args["module"].is_string()) {
            filter = args["module"].get<std::string>();
            std::transform(filter.begin(), filter.end(), filter.begin(),
                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        }
        BridgeList<Script::Function::FunctionInfo> list;
        if (!Script::Function::GetList(&list)) {
            r.ok=false; r.error="Script::Function::GetList failed"; return r;
        }
        nlohmann::json arr = nlohmann::json::array();
        const int n = list.Count();
        int kept = 0;
        for (int i = 0; i < n; ++i) {
            const auto& f = list[i];
            if (!filter.empty()) {
                std::string mod = f.mod;
                std::transform(mod.begin(), mod.end(), mod.begin(),
                               [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                if (mod.find(filter) == std::string::npos) continue;
            }
            arr.push_back({
                {"module",            f.mod},
                {"rva_start",         formatHexU64(static_cast<std::uint64_t>(f.rvaStart))},
                {"rva_end",           formatHexU64(static_cast<std::uint64_t>(f.rvaEnd))},
                {"manual",            f.manual},
                {"instruction_count", static_cast<std::uint64_t>(f.instructioncount)},
            });
            ++kept;
        }
        r.ok = true;
        r.data = {{"total", n}, {"returned", kept}, {"filter", filter}, {"functions", std::move(arr)}};
        return r;
    }
};

// ============= S6-E get_module_imports =============
class GetModuleImportsTool : public ITool {
public:
    std::string name() const override { return "get_module_imports"; }
    std::string description() const override
    {
        return "List IAT entries of a module (ordinal/name + IAT VA). Use to identify hooks, "
               "find which Win32 APIs the module relies on (crypto / net / file / anti-debug).";
    }
    std::string descriptionZh() const override
    {
        return "列出模块的 IAT 条目（ordinal/name + IAT VA）。用于识别 hook、判断该模块依赖哪些 Win32 API"
               "（加解密 / 网络 / 文件 / 反调试等）。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"module", {{"type","string"},{"description","Module name (e.g. 'kernel32.dll' or main exe name)"}}},
            }},
            {"required", nlohmann::json::array({"module"})},
        };
    }
    std::size_t maxResultBytes() const override { return 256 * 1024; }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        if (!args.contains("module") || !args["module"].is_string()) {
            r.ok=false; r.error="'module' required (string)"; return r;
        }
        const auto modName = args["module"].get<std::string>();
        Script::Module::ModuleInfo mi{};
        if (!Script::Module::InfoFromName(modName.c_str(), &mi)) {
            r.ok=false; r.error="module not loaded: " + modName; return r;
        }
        BridgeList<Script::Module::ModuleImport> list;
        if (!Script::Module::GetImports(&mi, &list)) {
            r.ok=false; r.error="Script::Module::GetImports failed for " + modName; return r;
        }
        nlohmann::json arr = nlohmann::json::array();
        const int n = list.Count();
        for (int i = 0; i < n; ++i) {
            const auto& im = list[i];
            arr.push_back({
                {"name",     im.name},
                {"undecorated", im.undecoratedName},
                {"ordinal",  static_cast<std::int64_t>(im.ordinal)},
                {"iat_va",   formatHexU64(static_cast<std::uint64_t>(im.iatVa))},
                {"iat_rva",  formatHexU64(static_cast<std::uint64_t>(im.iatRva))},
            });
        }
        r.ok = true;
        r.data = {{"module", modName}, {"base", formatHexU64(static_cast<std::uint64_t>(mi.base))},
                  {"count", n}, {"imports", std::move(arr)}};
        return r;
    }
};

// ============= S6-E get_module_exports =============
class GetModuleExportsTool : public ITool {
public:
    std::string name() const override { return "get_module_exports"; }
    std::string description() const override
    {
        return "List exported functions of a module (ordinal/name + VA + forward target). "
               "Use to find exposed API surface or forwarded symbols.";
    }
    std::string descriptionZh() const override
    {
        return "列出模块导出的函数（ordinal/name + VA + 转发目标）。"
               "用于查看对外暴露的 API 表面，或追踪 forward 到其它模块的符号。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"module", {{"type","string"},{"description","Module name (e.g. 'kernel32.dll')"}}},
            }},
            {"required", nlohmann::json::array({"module"})},
        };
    }
    std::size_t maxResultBytes() const override { return 256 * 1024; }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active"; return r;
        }
        if (!args.contains("module") || !args["module"].is_string()) {
            r.ok=false; r.error="'module' required (string)"; return r;
        }
        const auto modName = args["module"].get<std::string>();
        Script::Module::ModuleInfo mi{};
        if (!Script::Module::InfoFromName(modName.c_str(), &mi)) {
            r.ok=false; r.error="module not loaded: " + modName; return r;
        }
        BridgeList<Script::Module::ModuleExport> list;
        if (!Script::Module::GetExports(&mi, &list)) {
            r.ok=false; r.error="Script::Module::GetExports failed for " + modName; return r;
        }
        nlohmann::json arr = nlohmann::json::array();
        const int n = list.Count();
        for (int i = 0; i < n; ++i) {
            const auto& ex = list[i];
            nlohmann::json o = {
                {"name",         ex.name},
                {"undecorated",  ex.undecoratedName},
                {"ordinal",      static_cast<std::int64_t>(ex.ordinal)},
                {"va",           formatHexU64(static_cast<std::uint64_t>(ex.va))},
                {"rva",          formatHexU64(static_cast<std::uint64_t>(ex.rva))},
                {"forwarded",    ex.forwarded},
            };
            if (ex.forwarded) o["forward_name"] = ex.forwardName;
            arr.push_back(std::move(o));
        }
        r.ok = true;
        r.data = {{"module", modName}, {"base", formatHexU64(static_cast<std::uint64_t>(mi.base))},
                  {"count", n}, {"exports", std::move(arr)}};
        return r;
    }
};

void registerProgramMapTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<GetMemoryMapTool>(), "memory-search");
    reg.registerTool(std::make_unique<GetPageProtectTool>(), "memory-search");
    reg.registerTool(std::make_unique<SetPageProtectTool>(), "write-patch");
    reg.registerTool(std::make_unique<ListFunctionsTool>(), "disasm-cfg");
    reg.registerTool(std::make_unique<GetModuleImportsTool>(), "static-info");
    reg.registerTool(std::make_unique<GetModuleExportsTool>(), "static-info");
}

}  // namespace x64ai
