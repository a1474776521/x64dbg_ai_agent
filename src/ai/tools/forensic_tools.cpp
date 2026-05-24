// ai/tools/forensic_tools.cpp
//
// S8-B enum_handles          - 列被调试进程打开的内核句柄（含 NT 类型/对象名）
// S8-B enum_windows          - 列被调试进程线程拥有的窗口（标题/类/wndProc）
// S8-B enum_tcp_connections  - 列 TCP 连接（C2 分析必需；含 IPv6）
//
// 关键点：
//   EnumHandles 拿到 HANDLEINFO{Handle/TypeNumber/GrantedAccess} 之后**必须**再调
//   GetHandleName(handle, name, nameSize, typeName, typeNameSize) 才能拿到人类可读
//   的对象名/类型名（TypeNumber 是 NT 内核类型索引，跨系统版本不稳定）。
//
//   BridgeList<T> 用 operator&() 自动 Cleanup 旧数据 + 取 ListInfo* 地址；
//   作用域结束时析构自动 BridgeFree。
//
//   EnumWindows 枚举的是**被调试进程**的窗口，子窗口都会出现，可能很多 → 截断 1024。
//   wndProc 直接是 VA，方便配合 set_breakpoint 钩消息处理。
//
//   EnumTcpConnections 只覆盖 TCP（SDK 无 UDP）；StateText 已是可读字符串（"ESTABLISHED" 等）。
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
#include "bridgelist.h"
#include "_dbgfunctions.h"

#include "util/logging.h"

namespace x64ai {

namespace {

constexpr std::size_t kMaxHandles  = 4096;
constexpr std::size_t kMaxWindows  = 1024;
constexpr std::size_t kMaxTcp      = 1024;
constexpr std::size_t kNameBufSize = 512;  // GetHandleName 输出缓冲（典型 < 260）

std::string formatHexU64(std::uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
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

bool containsCI(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        if (iequals(hay.substr(i, needle.size()), needle)) return true;
    }
    return false;
}

}  // namespace

// ============= S8-B enum_handles =============
class EnumHandlesTool : public ITool {
public:
    std::string name() const override { return "enum_handles"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "Enumerate kernel handles opened by the debuggee. Each entry has "
               "handle, type (File/Mutant/Event/Key/...), name, granted_access. "
               "Optional 'type_filter' (case-insensitive substring). Capped at 4096.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"type_filter", {{"type","string"},{"description","Filter by type name substring, e.g. \"File\""}}},
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
        if (!fns || !fns->EnumHandles || !fns->GetHandleName) {
            r.ok=false; r.error="DbgFunctions->EnumHandles/GetHandleName is null"; return r;
        }

        std::string filter;
        if (args.contains("type_filter") && args["type_filter"].is_string()) {
            filter = args["type_filter"].get<std::string>();
        }

        BridgeList<HANDLEINFO> list;
        if (!fns->EnumHandles(&list)) {
            r.ok=false; r.error="EnumHandles failed"; return r;
        }
        const int total = list.Count();

        nlohmann::json arr = nlohmann::json::array();
        int emitted = 0;
        std::vector<char> nameBuf(kNameBufSize, 0);
        std::vector<char> typeBuf(kNameBufSize, 0);
        for (int i = 0; i < total; ++i) {
            const auto& h = list[i];

            // 第二阶段：拿 type / name
            std::memset(nameBuf.data(), 0, nameBuf.size());
            std::memset(typeBuf.data(), 0, typeBuf.size());
            const bool got = fns->GetHandleName(
                h.Handle, nameBuf.data(), nameBuf.size(),
                typeBuf.data(), typeBuf.size());
            const std::string typeStr = got ? std::string(typeBuf.data()) : std::string();
            const std::string nameStr = got ? std::string(nameBuf.data()) : std::string();

            if (!filter.empty() && !containsCI(typeStr, filter)) continue;
            if (emitted >= static_cast<int>(kMaxHandles)) break;

            arr.push_back({
                {"handle",         formatHexU64(static_cast<std::uint64_t>(h.Handle))},
                {"type_number",    static_cast<unsigned>(h.TypeNumber)},
                {"type",           typeStr},
                {"name",           nameStr},
                {"granted_access", formatHexU64(static_cast<std::uint64_t>(h.GrantedAccess))},
            });
            ++emitted;
        }

        XAI_LOG_INFO("enum_handles: total={} emitted={} filter=\"{}\"",
                     total, emitted, filter.c_str());
        r.ok=true;
        r.data = {
            {"total",     total},
            {"count",     emitted},
            {"truncated", emitted >= static_cast<int>(kMaxHandles)},
            {"filter",    filter},
            {"handles",   std::move(arr)},
        };
        return r;
    }
};

// ============= S8-B enum_windows =============
class EnumWindowsTool : public ITool {
public:
    std::string name() const override { return "enum_windows"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "Enumerate windows owned by debuggee threads. Each: hwnd, parent, "
               "tid, class, title, wnd_proc (VA), style, enabled. Use wnd_proc with "
               "set_breakpoint to hook the message handler. Capped at 1024.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"class_filter", {{"type","string"},{"description","Filter by window class substring (CI)"}}},
                {"title_filter", {{"type","string"},{"description","Filter by window title substring (CI)"}}},
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
        if (!fns || !fns->EnumWindows) {
            r.ok=false; r.error="DbgFunctions->EnumWindows is null"; return r;
        }

        std::string classFilter, titleFilter;
        if (args.contains("class_filter") && args["class_filter"].is_string())
            classFilter = args["class_filter"].get<std::string>();
        if (args.contains("title_filter") && args["title_filter"].is_string())
            titleFilter = args["title_filter"].get<std::string>();

        BridgeList<WINDOW_INFO> list;
        if (!fns->EnumWindows(&list)) {
            r.ok=false; r.error="EnumWindows failed"; return r;
        }
        const int total = list.Count();

        nlohmann::json arr = nlohmann::json::array();
        int emitted = 0;
        for (int i = 0; i < total; ++i) {
            const auto& w = list[i];
            const std::string cls(w.windowClass);
            const std::string ttl(w.windowTitle);
            if (!classFilter.empty() && !containsCI(cls, classFilter)) continue;
            if (!titleFilter.empty() && !containsCI(ttl, titleFilter)) continue;
            if (emitted >= static_cast<int>(kMaxWindows)) break;

            arr.push_back({
                {"hwnd",     formatHexU64(static_cast<std::uint64_t>(w.handle))},
                {"parent",   formatHexU64(static_cast<std::uint64_t>(w.parent))},
                {"tid",      static_cast<unsigned>(w.threadId)},
                {"class",    cls},
                {"title",    ttl},
                {"wnd_proc", formatHexU64(static_cast<std::uint64_t>(w.wndProc))},
                {"style",    formatHexU64(static_cast<std::uint64_t>(w.style))},
                {"style_ex", formatHexU64(static_cast<std::uint64_t>(w.styleEx))},
                {"enabled",  w.enabled},
            });
            ++emitted;
        }

        XAI_LOG_INFO("enum_windows: total={} emitted={}", total, emitted);
        r.ok=true;
        r.data = {
            {"total",     total},
            {"count",     emitted},
            {"truncated", emitted >= static_cast<int>(kMaxWindows)},
            {"windows",   std::move(arr)},
        };
        return r;
    }
};

// ============= S8-B enum_tcp_connections =============
class EnumTcpConnectionsTool : public ITool {
public:
    std::string name() const override { return "enum_tcp_connections"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "List TCP connections of the debuggee (IPv4/IPv6). Each entry has "
               "remote/local addr+port and state text (e.g. ESTABLISHED). UDP not supported.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {{"type","object"},{"properties", nlohmann::json::object()}};
    }
    ToolResult invoke(const nlohmann::json& /*args*/, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        const auto* fns = DbgFunctions();
        if (!fns || !fns->EnumTcpConnections) {
            r.ok=false; r.error="DbgFunctions->EnumTcpConnections is null"; return r;
        }

        BridgeList<TCPCONNECTIONINFO> list;
        if (!fns->EnumTcpConnections(&list)) {
            r.ok=false; r.error="EnumTcpConnections failed"; return r;
        }
        const int total = list.Count();
        const int emit  = total > static_cast<int>(kMaxTcp) ? static_cast<int>(kMaxTcp) : total;

        nlohmann::json arr = nlohmann::json::array();
        for (int i = 0; i < emit; ++i) {
            const auto& c = list[i];
            arr.push_back({
                {"remote",      std::string(c.RemoteAddress)},
                {"remote_port", static_cast<unsigned>(c.RemotePort)},
                {"local",       std::string(c.LocalAddress)},
                {"local_port",  static_cast<unsigned>(c.LocalPort)},
                {"state",       std::string(c.StateText)},
                {"state_code",  static_cast<unsigned>(c.State)},
            });
        }

        XAI_LOG_INFO("enum_tcp_connections: total={} emitted={}", total, emit);
        r.ok=true;
        r.data = {
            {"total",       total},
            {"count",       emit},
            {"truncated",   emit < total},
            {"connections", std::move(arr)},
        };
        return r;
    }
};

void registerForensicTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<EnumHandlesTool>());
    reg.registerTool(std::make_unique<EnumWindowsTool>());
    reg.registerTool(std::make_unique<EnumTcpConnectionsTool>());
}

}  // namespace x64ai
