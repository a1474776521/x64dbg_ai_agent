// trace/trace_recorder.h
//
// TraceRecorder：调用链事件录制器（单例）。
//
// 工作原理：
//   - 在 pluginInit 时挂载 CB_TRACEEXECUTE / CB_STARTTRACE / CB_STOPTRACE 回调
//   - 每个 trace step 进来后用 DbgDisasmFastAt 判定 call / ret
//   - call: 入队 Call 事件；ret: 入队 Ret 事件
//   - 内部用环形 buffer（cap 默认 20000）存事件，避免 OOM
//
// 工作模式：
//   - 被动监听（默认）：只要插件在线，用户在 x64dbg 自己 trace 时就能收到事件
//   - 主动启动：调用 startActive() 会自动发 "TraceIntoConditional 0" 命令
//
// 线程：
//   - CB_TRACEEXECUTE 在调试线程；本类内部加锁保护事件队列
//   - snapshot() 拷贝出事件副本供 UI 使用
#pragma once

#include "trace/trace_event.h"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace x64ai {

// 追溯模式
enum class TraceMode : uint8_t {
    // 目标导向：限定在某函数 [funcStart, funcEnd] 范围内录制；
    // 函数返回（CIP 跳出 + RSP > 进入时 RSP）后自动停止。
    Targeted = 0,
    // 全局：所有 trace 事件都录制（旧行为，仅供高级用户）
    Global   = 1,
};

// 启动追溯所需参数
struct TraceTarget {
    uint64_t funcStart  = 0;   // 函数首地址（DbgFunctionGet 归一化后）
    uint64_t funcEnd    = 0;   // 函数结束（exclusive）；0 表示未知
    uint64_t entryHit   = 0;   // 用户指定的命中地址（可能在函数中间）
    std::string label;         // 显示用，例如 "kernel32.CreateFileW"
};

class TraceRecorder {
public:
    static TraceRecorder& instance();

    void registerCallbacks(int pluginHandle);
    void unregisterCallbacks(int pluginHandle);

    // === 录制控制 ===
    // 目标导向：在 entryHit 处下一次性硬件断点，命中后自动 trace 直到 RSP/CIP 双判返回。
    // 返回 false 表示参数不合法或当前未在调试。
    bool startTargeted(const TraceTarget& target);

    // 全局录制：等价于旧的 startActive (TraceIntoConditional 0)
    void startGlobalActive();
    // 全局被动：仅打开开关，等用户自己 trace
    void startGlobalPassive();

    void stop();
    void clear();

    bool        isRecording()  const { return recording_.load(); }
    bool        hasArmed()     const { return armed_.load(); }
    TraceMode   mode()         const { return mode_; }
    TraceTarget target()       const;
    std::size_t eventCount()   const;
    std::size_t capacity()     const { return capacity_; }

    std::vector<TraceEvent> snapshot() const;

    using NotifyFn = std::function<void(std::size_t newCount)>;
    void setNotify(NotifyFn fn, std::size_t coalesce = 64);

    // 状态变更通知（armed/recording/stopped 等转变时触发，UI 可用来刷新按钮态）
    using StateFn = std::function<void()>;
    void setStateNotify(StateFn fn);

    // 解析用户输入为 VA：支持 "0x..." / 纯十六进制 / 函数名 / 模块.符号 / 表达式
    // 返回 0 表示解析失败。
    static uint64_t resolveAddressOrName(const std::string& input);

    // 用 DbgFunctionGet 把任意 VA 归一化到所属函数 [start,end]；失败返回 false。
    static bool normalizeToFunction(uint64_t va, uint64_t* start, uint64_t* end);

    // 回调入口
    void onTraceExecute(uint64_t cip);
    void onBreakpoint(uint64_t bpAddr);
    void onStartTrace();
    void onStopTrace();
    void onStopDebug();

private:
    TraceRecorder();
    ~TraceRecorder() = default;
    TraceRecorder(const TraceRecorder&)            = delete;
    TraceRecorder& operator=(const TraceRecorder&) = delete;

    void pushEvent(TraceEvent ev);
    void notifyState();
    static std::string resolveSymbol(uint64_t va);

    mutable std::mutex      mu_;
    std::deque<TraceEvent>  events_;
    std::size_t             capacity_ = 20000;
    std::atomic<uint64_t>   seqGen_{0};

    // 状态
    std::atomic<bool>       recording_{false};   // 是否正在记录事件
    std::atomic<bool>       armed_{false};       // Targeted 模式：已下断点等待命中
    TraceMode               mode_ = TraceMode::Targeted;
    TraceTarget             target_{};
    uint64_t                entrySp_ = 0;        // 目标命中时的 RSP；用于返回判定
    char                    entryModName_[256] = {0}; // 目标所属模块名（funcEnd=0 时兜底用）
    uint64_t                stepsSinceStart_ = 0;// trace 启动后已处理的 step 数
    uint64_t                startTickMs_ = 0;

    NotifyFn                notify_;
    std::size_t             notifyCoalesce_ = 64;
    std::size_t             notifyAccum_    = 0;

    StateFn                 stateNotify_;
};

}  // namespace x64ai
