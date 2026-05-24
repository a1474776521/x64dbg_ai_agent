// trace/trace_event.h
//
// 调用链追溯模块的公共事件结构。
//
// TraceEventKind: Call / Ret 两类事件
// TraceEvent:     由 TraceRecorder 在 CB_TRACEEXECUTE 中捕获生成
//
// 字段语义：
//   seq        全局递增序号（线程安全分配）
//   kind       Call / Ret
//   caller     发出 call 指令的地址（即 cip 本身）；ret 时也是 cip
//   callee     call 的目标地址；ret 时为 0
//   sp         事件发生时的 CSP（用于 call/ret 配对）
//   callerSym  caller 的可读符号（"module.symbol"），可空
//   calleeSym  callee 的可读符号，可空
//   timestampMs   毫秒级时间戳（QElapsedTimer 起算）
#pragma once

#include <cstdint>
#include <string>

namespace x64ai {

enum class TraceEventKind : uint8_t {
    Call = 0,
    Ret  = 1,
};

struct TraceEvent {
    uint64_t       seq         = 0;
    TraceEventKind kind        = TraceEventKind::Call;
    uint64_t       caller      = 0;
    uint64_t       callee      = 0;
    uint64_t       sp          = 0;
    std::string    callerSym;
    std::string    calleeSym;
    uint64_t       timestampMs = 0;
};

}  // namespace x64ai
