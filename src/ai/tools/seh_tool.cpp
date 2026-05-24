// ai/tools/seh_tool.cpp
//
// S8-C get_seh_chain - 列 32 位 SEH 异常处理链
//
// 关键点：
//   DBGSEHCHAIN { duint total; DBGSEHRECORD* records; }；records 由 BridgeAlloc 分配，
//   **必须** BridgeFree(records)。GetSEHChain 返回 void，无失败码，要先判 total > 0。
//
//   **仅 32 位有效**：x64 用 table-based SEH，链不存在，调用通常返回 total=0。
//   工具如实告知 LLM "is_x64=true 时 SEH 链通常为空，应改用 VEH/_C_specific_handler 分析"。
#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

#include <cstdio>
#include <string>
#include <vector>

#include <Windows.h>
#include "bridgemain.h"
#include "_dbgfunctions.h"

#include "util/logging.h"

namespace x64ai {

namespace {

std::string formatHexU64(std::uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace

class GetSehChainTool : public ITool {
public:
    std::string name() const override { return "get_seh_chain"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "Get 32-bit SEH exception handler chain (linked list at FS:[0]). "
               "On x64 returns empty (table-based SEH); inspect .pdata/_C_specific_handler instead.";
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
        if (!fns || !fns->GetSEHChain) {
            r.ok=false; r.error="DbgFunctions->GetSEHChain is null"; return r;
        }

        DBGSEHCHAIN chain = {};
        fns->GetSEHChain(&chain);  // void 返回

        const bool isX64 = (sizeof(duint) == 8);
        nlohmann::json arr = nlohmann::json::array();
        if (chain.total > 0 && chain.records != nullptr) {
            for (duint i = 0; i < chain.total; ++i) {
                arr.push_back({
                    {"frame",   formatHexU64(static_cast<std::uint64_t>(chain.records[i].addr))},
                    {"handler", formatHexU64(static_cast<std::uint64_t>(chain.records[i].handler))},
                });
            }
        }

        // 必须释放 records（即便 total=0 时 records 通常 nullptr，BridgeFree(nullptr) 安全）
        if (chain.records) BridgeFree(chain.records);

        XAI_LOG_INFO("get_seh_chain: count={} x64={}",
                     static_cast<unsigned long long>(chain.total), isX64);

        r.ok=true;
        r.data = {
            {"count",   static_cast<unsigned long long>(chain.total)},
            {"is_x64",  isX64},
            {"records", std::move(arr)},
        };
        if constexpr (sizeof(duint) == 8) {
            if (chain.total == 0) {
                r.data["hint"] = "x64 uses table-based SEH (.pdata/UNWIND_INFO + "
                                 "_C_specific_handler). Use list_modules + RtlLookupFunctionEntry "
                                 "or inspect .pdata section to enumerate handlers.";
            }
        }
        return r;
    }
};

void registerSehTool(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<GetSehChainTool>());
}

}  // namespace x64ai
