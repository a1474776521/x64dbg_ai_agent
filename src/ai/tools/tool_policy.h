// ai/tools/tool_policy.h
//
// S3-A：工具风险分档与默认 confirm 策略。
//
// 设计：
//   - 每个 ITool 子类静态属于一个 ToolCategory：
//       Read         纯只读（disasm/memory/registers/callstack/...）；不弹窗、不审计
//       DbgControl   控制执行流（step_in/step_over/run_until/wait_for_event）；不改持久状态但会让进程继续运行 → 写 audit，但不弹窗（agent 本就该自己等结果）
//       Write        改调试器/进程状态（set_bp/remove_bp/run_dbg_command）；写 audit + 默认弹 5s confirm
//
//   - 用户可通过 preset 字段 autoApprove: ["set_breakpoint", ...] 跳过 confirm；
//     但所有 Write/DbgControl 调用都必须落 audit，无法关闭。
//
//   - run_dbg_command 这种"半结构化"工具，confirm 流程**不**因 autoApprove 取消白名单检查；
//     白名单永远是硬约束。
#pragma once

#include <string>
#include <unordered_set>

namespace x64ai {

enum class ToolCategory {
    Read       = 0,  // 纯只读
    DbgControl = 1,  // 控制流（step/run/wait）
    Write      = 2,  // 改状态（断点/dbg cmd/未来的 patch_memory）
};

inline const char* toolCategoryName(ToolCategory c)
{
    switch (c) {
        case ToolCategory::Read:       return "read";
        case ToolCategory::DbgControl: return "dbg_control";
        case ToolCategory::Write:      return "write";
    }
    return "unknown";
}

// 调用方在每次 dispatch 前传入此结构，描述"本次调用是否需要弹 confirm"。
// 由 ToolRegistry 根据 ITool::category() + ITool::requiresUserConfirmation()
// + ToolContext 里的 autoApprove 集合综合判断。
struct ToolPolicyDecision {
    bool needConfirm  = false;   // 是否要触发 ToolConfirmDialog
    bool writeAudit   = false;   // 是否要写 write_audit.log
    bool blockedByAcl = false;   // 例如 run_dbg_command 未通过白名单 → 直接 ok=false
    std::string blockReason;     // blockedByAcl=true 时填友好原因
};

}  // namespace x64ai
