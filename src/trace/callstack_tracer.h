// trace/callstack_tracer.h
//
// CallStackTracer：调用栈反向追溯（单例）。
//
// 用途：
//   在某个目标地址（典型如 ws2_32.send / WSASend / NtWriteFile）下断点，
//   每次命中时抓取当前线程的完整调用栈（DbgFunctions->GetCallStack），
//   按"完整路径"去重累计，UI 上反向展示"谁调了我"。
//
// 与 TraceRecorder 的区别：
//   TraceRecorder = 正向 step trace（看函数内部往下调用什么）
//   CallStackTracer = 反向 unwind（看函数被谁调用，常用于发包/IO 反查组包逻辑）
//
// 工作流程：
//   1. armWith(addr, label, maxSamples)：在 addr 下软断点
//   2. 命中 -> onBreakpoint(addr) -> 调用 captureOnce()
//      - 抓 callstack -> 序列化为 vector<Frame>
//      - 按 hash 去重，hits++
//      - 达到 maxSamples 后自动 stop（拆断点 + run）
//   3. UI 调 snapshot() 取所有去重后的 stack 列表
//
// 线程：
//   onBreakpoint 在调试线程；mu_ 保护 samples_。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace x64ai {

struct CallStackFrame {
    uint64_t    addr   = 0;   // 该栈帧的返回地址（即 caller 中的下一条指令）
    uint64_t    from   = 0;   // 调用源指令地址（caller 里的 call 指令）
    uint64_t    to     = 0;   // 被调函数入口
    std::string symbol;       // x64dbg 提供的注释（含模块.符号）
};

struct CallStackSample {
    std::vector<CallStackFrame> frames;   // [0]=最内层(被断的函数自身)，[N-1]=最外层(线程入口)
    uint32_t                    hits = 1; // 命中次数（去重累计）
    uint64_t                    firstSeq = 0;
    uint64_t                    timestampMs = 0;
};

class CallStackTracer {
public:
    static CallStackTracer& instance();

    // 在 addr 处下软断点，最多采样 maxSamples 次后自动拆断点。
    // label 仅用于 UI 显示。返回 false 表示参数非法 / 未在调试 / 下断失败。
    bool armWith(uint64_t addr, const std::string& label, uint32_t maxSamples = 32);

    // 主动停止：拆断点（如果还在），不再采样
    void stop();

    void clear();

    bool        isArmed()      const { return armed_.load(); }
    uint64_t    armedAddress() const { return armedAddr_.load(); }
    std::string label()        const;
    uint32_t    sampleCount()  const;     // 已采样总次数（含重复）
    uint32_t    uniqueCount()  const;     // 去重后 stack 数
    uint32_t    maxSamples()   const { return maxSamples_; }

    // 拷贝当前去重后的 stack 列表（按 hits 降序）
    std::vector<CallStackSample> snapshot() const;

    // 由 plugin_callbacks 在 CB_BREAKPOINT 触发时分发
    void onBreakpoint(uint64_t bpAddr);
    void onStopDebug();

    using NotifyFn = std::function<void()>;
    void setNotify(NotifyFn fn);   // 任一采样完成 / 自动停止时触发，UI 用来刷新

private:
    CallStackTracer();
    ~CallStackTracer() = default;
    CallStackTracer(const CallStackTracer&)            = delete;
    CallStackTracer& operator=(const CallStackTracer&) = delete;

    void captureOnce();
    void disarm();   // 拆断点 + armed=false（不影响 samples_）

    static uint64_t hashFrames(const std::vector<CallStackFrame>& f);

    mutable std::mutex            mu_;
    std::vector<CallStackSample>  samples_;   // 去重后的样本
    std::atomic<bool>             armed_{false};
    std::atomic<uint64_t>         armedAddr_{0};
    std::string                   label_;
    uint32_t                      maxSamples_ = 32;
    uint32_t                      totalHits_  = 0;
    uint64_t                      seqGen_     = 0;
    uint64_t                      startTickMs_= 0;

    NotifyFn                      notify_;
};

}  // namespace x64ai
