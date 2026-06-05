// ai/tools/injection_stack_tools.cpp
//
// S8-D remote_alloc / remote_free - 在被调试进程地址空间分配/释放虚拟内存
// S8-D stack_push                  - 把 duint 压栈（ESP/RSP -= ptrsize）
// S8-D stack_peek                  - 读栈顶相对偏移的 duint，不改变栈
//
// 关键点：
//   Script::Memory::RemoteAlloc(addr, size)：addr=0 让系统选地址；保护属性源码内**固定** PAGE_EXECUTE_READWRITE。
//     若需 RW 后再改 X，调 set_page_protect 二次修改。
//   Script::Memory::RemoteFree(addr)：只接受 RemoteAlloc 返回的**基址**，不能传中间页。
//
//   Script::Stack::Push(value)：返回 push 之前的栈顶值（≈ Peek(0)）；x86 ESP-=4, x64 RSP-=8。
//   Script::Stack::Peek(offset)：**offset 单位 = Register::Size() 的倍数**（x86=4字节, x64=8字节），
//     **不是字节**。这是 SDK 最常见的坑！工具描述里务必标红。
//
//   注意：stack_pop（真弹出）反向工程场景几乎不用，且会破坏调用约定的 ESP 一致性，因此**不暴露 pop**。
//   想要弹出 + 恢复，让 LLM 用 stack_peek 读 + set_register("esp", esp+size) 显式做。
#include "ai/tools/builtin_tools.h"
#include "ai/tools/dbg_state_util.h"  // K-43
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

#include <cstdio>
#include <string>

#include <Windows.h>
#include "bridgemain.h"
#include "_scriptapi_memory.h"
#include "_scriptapi_stack.h"
#include "_scriptapi_register.h"

#include "util/logging.h"

namespace x64ai {

namespace {

constexpr std::size_t kMaxRemoteAlloc = 64 * 1024 * 1024;  // 64 MB 上限，防止 LLM 误传巨大 size

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

// ============= S8-D remote_alloc =============
class RemoteAllocTool : public ITool {
public:
    std::string name() const override { return "remote_alloc"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Allocate virtual memory in the debuggee. addr=0 lets system choose. "
               "Protection is fixed PAGE_EXECUTE_READWRITE (RWX) by SDK; use "
               "set_page_protect to change after. size<=64MB. Returns the base VA.";
    }
    std::string descriptionZh() const override
    {
        return "在被调试进程中分配一段虚拟内存。addr=0 表示由系统选址。"
               "由 SDK 限制保护属性固定为 PAGE_EXECUTE_READWRITE（RWX），可事后用 set_page_protect 修改。"
               "size ≤ 64MB。返回分配区的基址 VA。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"addr", {{"type","string"},{"description","Suggested base VA (0/omit = system chooses)"}}},
                {"size", {{"type","string"},{"description","Bytes to allocate, decimal or 0x hex; <=64MB"}}},
            }},
            {"required", nlohmann::json::array({"size"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        std::uint64_t addr = 0, size = 0;
        std::string e;
        if (args.contains("addr") && !args["addr"].is_null()) {
            if (!parseVa(args, "addr", addr, e)) { r.ok=false; r.error=e; return r; }
        }
        if (!parseVa(args, "size", size, e)) { r.ok=false; r.error=e; return r; }
        if (size == 0) { r.ok=false; r.error="'size' must be > 0"; return r; }
        if (size > kMaxRemoteAlloc) {
            r.ok=false;
            r.error="'size' exceeds 64MB safety cap";
            return r;
        }

        const duint base = Script::Memory::RemoteAlloc(
            static_cast<duint>(addr), static_cast<duint>(size));
        if (base == 0) {
            r.ok=false;
            r.error="RemoteAlloc failed (insufficient memory, conflicting address, or RWX denied)";
            return r;
        }

        XAI_LOG_INFO("remote_alloc: base={} size=0x{:X}",
                     formatHexU64(base).c_str(),
                     static_cast<unsigned long long>(size));
        r.ok=true;
        r.data = {
            {"base",       formatHexU64(static_cast<std::uint64_t>(base))},
            {"size",       formatHexU64(size)},
            {"protection", "PAGE_EXECUTE_READWRITE"},
        };
        return r;
    }
};

// ============= S8-D remote_free =============
class RemoteFreeTool : public ITool {
public:
    std::string name() const override { return "remote_free"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Free a region previously returned by remote_alloc. addr MUST be "
               "the base, not a mid-region pointer. Returns ok=false on bad base.";
    }
    std::string descriptionZh() const override
    {
        return "释放之前由 remote_alloc 分配的区域。addr 必须是基址，不能是区域中间的指针。"
               "基址不正确时返回 ok=false。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"addr", {{"type","string"},{"description","Base VA from remote_alloc"}}},
            }},
            {"required", nlohmann::json::array({"addr"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        std::uint64_t addr = 0;
        std::string e;
        if (!parseVa(args, "addr", addr, e)) { r.ok=false; r.error=e; return r; }
        const bool ok = Script::Memory::RemoteFree(static_cast<duint>(addr));
        if (!ok) {
            r.ok=false;
            r.error="RemoteFree failed (addr is not a region base, or VirtualFreeEx failed)";
            return r;
        }
        XAI_LOG_INFO("remote_free: addr={}", formatHexU64(addr).c_str());
        r.ok=true;
        r.data = {{"addr", formatHexU64(addr)}};
        return r;
    }
};

// ============= S8-D stack_push =============
class StackPushTool : public ITool {
public:
    std::string name() const override { return "stack_push"; }
    ToolCategory category() const override { return ToolCategory::Write; }
    bool requiresUserConfirmation() const override { return true; }
    std::string description() const override
    {
        return "Push a duint value onto the debuggee stack (ESP/RSP -= pointer_size). "
               "Use for fake return addresses or argument injection. Returns the "
               "previous top-of-stack value.";
    }
    std::string descriptionZh() const override
    {
        return "向被调试进程栈上压入一个 duint 值（ESP/RSP -= 指针大小）。"
               "可用于伪造返回地址或注入参数。返回原栈顶值。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"value", {{"type","string"},{"description","Value to push, decimal or 0x hex"}}},
            }},
            {"required", nlohmann::json::array({"value"})},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        // K-43: stack_push 实际改 SetThreadContext(SP) + WriteProcessMemory(SP-N)。
        // running 时 SP 是缓存历史值，按它算出来的地址几乎必定不是当前真实栈位置——
        // 轻则写到无意义区域、重则覆盖随机数据让 debuggee 立刻 crash。
        // 必须 paused 才允许。
        if (DbgIsRunning()) {
            r.ok = false;
            r.error = "cannot stack_push: debuggee is currently 'running'; SP is not "
                      "stable (the cached value reflects the last paused snapshot, not "
                      "the live thread). Writing through stale SP can corrupt random "
                      "memory and crash the debuggee. Call pause_debug first.";
            r.data = {{"current_state", "running"}};
            return r;
        }
        std::uint64_t v = 0;
        std::string e;
        if (!parseVa(args, "value", v, e)) { r.ok=false; r.error=e; return r; }
        const duint prev = Script::Stack::Push(static_cast<duint>(v));
        const duint newSp = Script::Register::GetCSP();  // CSP = ESP/RSP 的完整 duint
        XAI_LOG_INFO("stack_push: value={} new_sp={}",
                     formatHexU64(v).c_str(),
                     formatHexU64(newSp).c_str());
        r.ok=true;
        r.data = {
            {"pushed_value",       formatHexU64(v)},
            {"prev_top_of_stack",  formatHexU64(static_cast<std::uint64_t>(prev))},
            {"new_sp",             formatHexU64(static_cast<std::uint64_t>(newSp))},
            {"pointer_size",       static_cast<unsigned>(sizeof(duint))},
        };
        return r;
    }
};

// ============= S8-D stack_peek =============
class StackPeekTool : public ITool {
public:
    std::string name() const override { return "stack_peek"; }
    ToolCategory category() const override { return ToolCategory::Read; }
    bool requiresUserConfirmation() const override { return false; }
    std::string description() const override
    {
        return "Read a duint from the stack at [SP + offset * pointer_size]. "
               "offset is in POINTER-SIZED SLOTS not bytes (offset=1 means +4 on x86, +8 on x64). "
               "Default offset=0 (top of stack). Use to inspect return addr/args without altering SP.";
    }
    std::string descriptionZh() const override
    {
        return "从栈上 [SP + offset * 指针大小] 处读取一个 duint。"
               "offset 单位是指针槽，不是字节（offset=1 表示 x86 +4 / x64 +8）。"
               "默认 offset=0（栈顶）。用于查看返回地址/参数而不动 SP。";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type","object"},
            {"properties", {
                {"offset", {{"type","integer"},{"description","Slot offset from SP (slots, not bytes); default 0"}}},
            }},
        };
    }
    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok=false; r.error="debugger is not active"; return r;
        }
        int off = 0;
        if (args.contains("offset")) {
            std::string e;
            if (!parseInt32Lenient(args["offset"], -1024, 1024, off, e)) {
                r.ok=false; r.error=e; return r;
            }
        }
        const duint val = Script::Stack::Peek(off);
        const duint sp  = Script::Register::GetCSP();
        const std::uint64_t va = static_cast<std::uint64_t>(sp) +
            static_cast<std::int64_t>(off) * static_cast<std::int64_t>(sizeof(duint));
        // K-43: running 时 SP 和 peek 出的值都是缓存历史值。不拒绝（读类无副作用），
        // 但 stale 字段直接告诉 LLM 别拿这些数据做精确决策。
        const bool stale = DbgIsRunning();
        r.ok=true;
        r.data = {
            {"sp",            formatHexU64(static_cast<std::uint64_t>(sp))},
            {"offset_slots",  off},
            {"address",       formatHexU64(va)},
            {"value",         formatHexU64(static_cast<std::uint64_t>(val))},
            {"pointer_size",  static_cast<unsigned>(sizeof(duint))},
            {"current_state", currentDbgStateStr()},
            {"stale",         stale},
        };
        if (stale) {
            r.data["stale_note"] =
                "debuggee is currently running; SP/value reflect the last paused snapshot, "
                "not live thread state. Call pause_debug + stack_peek again if you need live data.";
        }
        return r;
    }
};

void registerInjectionStackTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<RemoteAllocTool>(), "write-patch");
    reg.registerTool(std::make_unique<RemoteFreeTool>(), "write-patch");
    reg.registerTool(std::make_unique<StackPushTool>(), "write-patch");
    reg.registerTool(std::make_unique<StackPeekTool>(), "register-stack");
}

}  // namespace x64ai
