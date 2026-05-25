// ai/tools/anti_debug_tools.cpp
//
// S8-A list_threads       - 线程枚举（含 TEB / CIP / 等待原因 / 优先级 / LastError）
// S8-A get_peb_address    - 拿被调试进程的 PEB VA
// S8-A get_anti_debug_flags - 读 PEB 内常被反调试 API 检测的字段（BeingDebugged / NtGlobalFlag / ProcessHeap）
//
// 设计要点：
//   list_threads 用 DbgGetThreadList(THREADLIST*)；list.list 由 BridgeAlloc 分配，
//   **必须** 手动 BridgeFree(list.list)；CurrentThread 是 0-based 索引而非 TID。
//
//   get_peb_address 直接调 DbgGetPebAddress(DbgGetProcessId())，不需 attach handle。
//
//   get_anti_debug_flags 不再额外封 helper，而是组合 get_peb_address + read_memory 三个字段：
//     - PEB.BeingDebugged @ +0x02 (BYTE)
//     - PEB.NtGlobalFlag  @ +0x68 (x86) / +0xBC (x64) (DWORD) —— FLG_HEAP_ENABLE_TAIL_CHECK 等
//     - PEB.ProcessHeap   @ +0x18 (x86) / +0x30 (x64) (duint)  —— 上层用于 HeapFlags 进一步检查
//   我们只读上述三个稳定字段；HeapFlags/HeapForceFlags 偏移版本差异大（Win7/8/10/11 不一致），
//   不在工具层硬编码，由 LLM 拿到 ProcessHeap 后自己用 read_memory 探测。
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

constexpr std::size_t kMaxThreads = 1024;  // 真有 1024 线程的样本已经够 LLM 头大

std::string formatHexU64(std::uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

const char* priorityToString(THREADPRIORITY p)
{
    switch (p) {
        case _PriorityIdle:           return "Idle";
        case _PriorityAboveNormal:    return "AboveNormal";
        case _PriorityBelowNormal:    return "BelowNormal";
        case _PriorityHighest:        return "Highest";
        case _PriorityLowest:         return "Lowest";
        case _PriorityNormal:         return "Normal";
        case _PriorityTimeCritical:   return "TimeCritical";
        default:                      return "Unknown";
    }
}

const char* waitReasonToString(THREADWAITREASON w)
{
    // 只翻译最常见的，其余给 raw 数值
    switch (w) {
        case _Executive:        return "Executive";
        case _DelayExecution:   return "DelayExecution";
        case _Suspended:        return "Suspended";
        case _UserRequest:      return "UserRequest";
        case _WrUserRequest:    return "WrUserRequest";
        case _WrEventPair:      return "WrEventPair";
        case _WrQueue:          return "WrQueue";
        case _WrLpcReceive:     return "WrLpcReceive";
        case _WrLpcReply:       return "WrLpcReply";
        case _WrYieldExecution: return "WrYieldExecution";
        default:                return nullptr;  // 调用方填 raw
    }
}

}  // namespace

// ============= S8-A list_threads =============
class ListThreadsTool : public ITool {
public:
    std::string name() const override { return "list_threads"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "List all threads of the debuggee: id, TEB, CIP, suspend, priority, "
               "wait reason, LastError, thread name. 'current' is the active thread id.";
    }
    std::string descriptionZh() const override
    {
        return "列出被调试进程的所有线程：id、TEB、CIP、挂起计数、优先级、"
               "等待原因、LastError、线程名。返回中的 'current' 为当前活动线程 id。";
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

        THREADLIST list = {};
        DbgGetThreadList(&list);  // void 返回；list.list 由 BridgeAlloc 分配
        if (list.count == 0 || list.list == nullptr) {
            // 释放（即便 count==0 也可能 data 为 nullptr，BridgeFree(nullptr) 安全）
            if (list.list) BridgeFree(list.list);
            r.ok=true;
            r.data = {{"count",0},{"threads", nlohmann::json::array()},{"current_thread_id", 0}};
            return r;
        }

        nlohmann::json arr = nlohmann::json::array();
        const int total = list.count;
        const int emit  = total > static_cast<int>(kMaxThreads) ? static_cast<int>(kMaxThreads) : total;
        DWORD currentTid = 0;
        if (list.CurrentThread >= 0 && list.CurrentThread < list.count) {
            currentTid = list.list[list.CurrentThread].BasicInfo.ThreadId;
        }

        for (int i = 0; i < emit; ++i) {
            const auto& t = list.list[i];
            nlohmann::json j;
            j["index"]              = i;
            j["thread_id"]          = static_cast<unsigned>(t.BasicInfo.ThreadId);
            j["teb"]                = formatHexU64(static_cast<std::uint64_t>(t.BasicInfo.ThreadLocalBase));
            j["start_address"]      = formatHexU64(static_cast<std::uint64_t>(t.BasicInfo.ThreadStartAddress));
            j["cip"]                = formatHexU64(static_cast<std::uint64_t>(t.ThreadCip));
            j["suspend_count"]      = static_cast<unsigned>(t.SuspendCount);
            j["priority"]           = priorityToString(t.Priority);
            const char* wn = waitReasonToString(t.WaitReason);
            j["wait_reason"]        = wn ? std::string(wn) : std::to_string(static_cast<int>(t.WaitReason));
            j["last_error"]         = static_cast<unsigned>(t.LastError);
            if (t.BasicInfo.threadName[0] != '\0') {
                j["name"] = std::string(t.BasicInfo.threadName);
            }
            arr.push_back(std::move(j));
        }

        BridgeFree(list.list);

        XAI_LOG_INFO("list_threads: total={} emitted={} current_tid={}",
                     total, emit, static_cast<unsigned long long>(currentTid));
        r.ok=true;
        r.data = {
            {"count",             emit},
            {"total",             total},
            {"truncated",         emit < total},
            {"current_thread_id", static_cast<unsigned>(currentTid)},
            {"threads",           std::move(arr)},
        };
        return r;
    }
};

// ============= S8-A get_peb_address =============
class GetPebAddressTool : public ITool {
public:
    std::string name() const override { return "get_peb_address"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "Return the PEB virtual address of the debuggee. Pair with read_memory "
               "to inspect PEB fields. On WOW64 returns the 64-bit PEB.";
    }
    std::string descriptionZh() const override
    {
        return "返回被调试进程的 PEB 虚拟地址。配合 read_memory 用于检查 PEB 字段。"
               "WOW64 进程下返回 64 位 PEB 地址。";
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
        const DWORD pid = DbgGetProcessId();
        if (pid == 0) { r.ok=false; r.error="DbgGetProcessId returned 0"; return r; }
        const duint peb = DbgGetPebAddress(pid);
        if (peb == 0) { r.ok=false; r.error="DbgGetPebAddress returned 0"; return r; }
        r.ok=true;
        r.data = {
            {"pid",          static_cast<unsigned>(pid)},
            {"peb",          formatHexU64(static_cast<std::uint64_t>(peb))},
            {"pointer_size", static_cast<unsigned>(sizeof(duint))},
        };
        return r;
    }
};

// ============= S8-A get_anti_debug_flags =============
class GetAntiDebugFlagsTool : public ITool {
public:
    std::string name() const override { return "get_anti_debug_flags"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "Read commonly-checked anti-debug fields from PEB: BeingDebugged (BYTE @+2), "
               "NtGlobalFlag (DWORD @+0x68/x86 or +0xBC/x64), ProcessHeap pointer. "
               "Use ProcessHeap + read_memory to inspect HeapFlags further (offsets vary by Win version).";
    }
    std::string descriptionZh() const override
    {
        return "读取 PEB 中常见的反调试相关字段：BeingDebugged（+2 字节）、"
               "NtGlobalFlag（x86 @+0x68 / x64 @+0xBC 的 DWORD）、ProcessHeap 指针。"
               "如需进一步检查 HeapFlags，用 ProcessHeap + read_memory（偏移因 Windows 版本而异）。";
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
        const DWORD pid = DbgGetProcessId();
        if (pid == 0) { r.ok=false; r.error="DbgGetProcessId returned 0"; return r; }
        const duint peb = DbgGetPebAddress(pid);
        if (peb == 0) { r.ok=false; r.error="DbgGetPebAddress returned 0"; return r; }

        // BeingDebugged @ PEB+0x02 (BYTE)
        BYTE beingDebugged = 0;
        if (!DbgMemRead(peb + 0x02, &beingDebugged, sizeof(beingDebugged))) {
            r.ok=false; r.error="DbgMemRead(BeingDebugged) failed"; return r;
        }

        // NtGlobalFlag: x86 @ +0x68, x64 @ +0xBC
        const duint ntGlobalFlagOff = (sizeof(duint) == 8) ? 0xBC : 0x68;
        DWORD ntGlobalFlag = 0;
        if (!DbgMemRead(peb + ntGlobalFlagOff, &ntGlobalFlag, sizeof(ntGlobalFlag))) {
            r.ok=false; r.error="DbgMemRead(NtGlobalFlag) failed"; return r;
        }

        // ProcessHeap: x86 @ +0x18, x64 @ +0x30
        const duint processHeapOff = (sizeof(duint) == 8) ? 0x30 : 0x18;
        duint processHeap = 0;
        if (!DbgMemRead(peb + processHeapOff, &processHeap, sizeof(processHeap))) {
            r.ok=false; r.error="DbgMemRead(ProcessHeap) failed"; return r;
        }

        // NtGlobalFlag 解码（常见反调试位）
        nlohmann::json flagsArr = nlohmann::json::array();
        if (ntGlobalFlag & 0x10) flagsArr.push_back("FLG_HEAP_ENABLE_TAIL_CHECK");
        if (ntGlobalFlag & 0x20) flagsArr.push_back("FLG_HEAP_ENABLE_FREE_CHECK");
        if (ntGlobalFlag & 0x40) flagsArr.push_back("FLG_HEAP_VALIDATE_PARAMETERS");
        const bool antiDebugHeapFlagsSet = (ntGlobalFlag & 0x70) != 0;  // 三位组合 = 反调试常用模式

        XAI_LOG_INFO("get_anti_debug_flags: peb={} bd={} ntglobalflag=0x{:X} processheap={}",
                     formatHexU64(peb).c_str(),
                     static_cast<unsigned>(beingDebugged),
                     static_cast<unsigned>(ntGlobalFlag),
                     formatHexU64(processHeap).c_str());

        r.ok=true;
        r.data = {
            {"peb",                       formatHexU64(static_cast<std::uint64_t>(peb))},
            {"being_debugged",            static_cast<unsigned>(beingDebugged)},
            {"nt_global_flag",            formatHexU64(static_cast<std::uint64_t>(ntGlobalFlag))},
            {"nt_global_flag_decoded",    std::move(flagsArr)},
            {"anti_debug_heap_flags_set", antiDebugHeapFlagsSet},
            {"process_heap",              formatHexU64(static_cast<std::uint64_t>(processHeap))},
            {"pointer_size",              static_cast<unsigned>(sizeof(duint))},
            {"hint",                      "If anti_debug_heap_flags_set==true OR being_debugged!=0, "
                                          "the process likely detects a debugger. Patch PEB.BeingDebugged "
                                          "to 0 and NtGlobalFlag clear bits 0x70 to bypass simple checks."},
        };
        return r;
    }
};

void registerAntiDebugTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<ListThreadsTool>(), "anti-debug-insight");
    reg.registerTool(std::make_unique<GetPebAddressTool>(), "anti-debug-insight");
    reg.registerTool(std::make_unique<GetAntiDebugFlagsTool>(), "anti-debug-insight");
}

}  // namespace x64ai
