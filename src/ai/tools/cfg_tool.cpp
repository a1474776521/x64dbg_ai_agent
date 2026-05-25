// ai/tools/cfg_tool.cpp
//
// S7-E get_cfg - 分析函数并输出 Mermaid 流程图
//
// 数据来源：bridgemain.h:1238 DbgAnalyzeFunction(entry, BridgeCFGraphList*)
//          bridgegraph.h C++ wrapper: BridgeCFGraph(list, freedata=true) 自动 ToVector + Free
//
// 输出策略（与用户确认：Mermaid 单一格式）：
//   graph TD
//     N_<hex>["<hex_start>..<hex_end>\n<icount> insn[ RET][ ICALL][ SPLIT]"]
//     N_a --> N_b      (uncondit / split fallthrough)
//     N_a -->|T| N_b   (brtrue, condit)
//     N_a -->|F| N_c   (brfalse, condit)
//   class N_<entry> entry;
//   classDef entry fill:#fcc,stroke:#900
//
// 规模保护：节点数 > 256 时截断，避免 mermaid 渲染崩溃（前端 mermaid.js 默认 maxEdges=500）
// 仅 Read 类（不修改任何状态）。
#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

#include <cstdio>
#include <sstream>
#include <string>

#include <Windows.h>
#include "bridgemain.h"
#include "bridgegraph.h"

#include "util/logging.h"

namespace x64ai {

namespace {

constexpr std::size_t kMaxNodes = 256;

std::string hex(duint v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%llX", static_cast<unsigned long long>(v));
    return buf;
}
std::string hex0x(duint v)
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

class GetCfgTool : public ITool {
public:
    std::string name() const override { return "get_cfg"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "Analyze the function at entry VA and return a Mermaid 'graph TD' of its CFG. "
               "Each node shows start..end / icount; edges labeled T/F for conditional branches. "
               "Truncated at 256 nodes.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"entry", {{"type","string"},{"description","Function entry VA, decimal or 0x hex"}}},
            }},
            {"required", nlohmann::json::array({"entry"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        std::uint64_t entry = 0; std::string e;
        if (!parseVa(args, "entry", entry, e)) { r.ok=false; r.error=e; return r; }

        BridgeCFGraphList list{};
        if (!DbgAnalyzeFunction(static_cast<duint>(entry), &list)) {
            r.ok=false; r.error="DbgAnalyzeFunction failed (entry not inside a known function?)";
            return r;
        }
        // C++ wrapper 接管 list 内存（freedata=true 会 BridgeFree 节点数组）
        BridgeCFGraph g(&list, /*freedata=*/true);

        const std::size_t total = g.nodes.size();
        const bool truncated = total > kMaxNodes;

        std::ostringstream os;
        os << "graph TD\n";

        // 节点定义
        std::size_t count = 0;
        for (const auto& kv : g.nodes) {
            if (count++ >= kMaxNodes) break;
            const auto& n = kv.second;
            os << "    N_" << hex(n.start) << "[\""
               << hex0x(n.start) << "..." << hex0x(n.end)
               << "\\n" << n.icount << " insn";
            if (n.terminal)     os << " RET";
            if (n.indirectcall) os << " ICALL";
            if (n.split)        os << " SPLIT";
            os << "\"]\n";
        }

        // 边
        count = 0;
        for (const auto& kv : g.nodes) {
            if (count++ >= kMaxNodes) break;
            const auto& n = kv.second;
            const bool hasT = n.brtrue  != 0;
            const bool hasF = n.brfalse != 0;
            if (hasT && hasF) {
                os << "    N_" << hex(n.start) << " -->|T| N_" << hex(n.brtrue)  << "\n";
                os << "    N_" << hex(n.start) << " -->|F| N_" << hex(n.brfalse) << "\n";
            } else if (hasT) {
                os << "    N_" << hex(n.start) << " --> N_" << hex(n.brtrue) << "\n";
            } else if (hasF) {
                os << "    N_" << hex(n.start) << " --> N_" << hex(n.brfalse) << "\n";
            }
            // 无出边 = 终止块（RET/JMP reg/etc）
        }

        // 入口节点高亮
        os << "    classDef entry fill:#fcc,stroke:#900\n";
        os << "    class N_" << hex(g.entryPoint) << " entry\n";

        XAI_LOG_INFO("get_cfg: entry={} nodes={} truncated={}",
                     hex0x(g.entryPoint).c_str(),
                     static_cast<unsigned long long>(total), truncated);

        r.ok = true;
        r.data = {
            {"entry",     hex0x(g.entryPoint)},
            {"nodes",     total},
            {"truncated", truncated},
            {"mermaid",   os.str()},
        };
        return r;
    }
};

void registerCfgTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<GetCfgTool>(), "disasm-cfg");
}

}  // namespace x64ai
