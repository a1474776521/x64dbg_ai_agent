// trace/trace_recorder.cpp
#include "trace/trace_recorder.h"

#include <Windows.h>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "_plugins.h"
#include "bridgemain.h"
#include "_scriptapi_register.h"

#include "trace/callstack_tracer.h"
#include "util/logging.h"

namespace x64ai {

namespace {

constexpr int kNotifyDefaultCoalesce = 64;

uint64_t nowMs()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void cbTraceExecute(CBTYPE, void* info)
{
    auto* p = reinterpret_cast<PLUG_CB_TRACEEXECUTE*>(info);
    if (!p) return;
    TraceRecorder::instance().onTraceExecute(static_cast<uint64_t>(p->cip));
}

void cbBreakpoint(CBTYPE, void* info)
{
    auto* p = reinterpret_cast<PLUG_CB_BREAKPOINT*>(info);
    if (!p || !p->breakpoint) return;
    uint64_t addr = static_cast<uint64_t>(p->breakpoint->addr);
    // 同一 CBTYPE 在 x64dbg 内部被插件后注册者覆盖，因此这里集中分发
    TraceRecorder::instance().onBreakpoint(addr);
    CallStackTracer::instance().onBreakpoint(addr);
}

void cbStartTrace(CBTYPE, void*)  { TraceRecorder::instance().onStartTrace(); }
void cbStopTrace(CBTYPE, void*)   { TraceRecorder::instance().onStopTrace();  }
// S0-C1 (2026-05-24)：原本这里还有一个 cbStopDebug 注册 CB_STOPDEBUG，
// 与 plugin_callbacks 的同名回调冲突（后注册者覆盖前者），导致
// AssistantPanel::onDebugStopped / ProjectContext::onDebugStop 丢失。
// 现已迁移到 plugin_callbacks::cbStopDebug 单点分发，本处不再注册。

}  // namespace

TraceRecorder& TraceRecorder::instance()
{
    static TraceRecorder inst;
    return inst;
}

TraceRecorder::TraceRecorder() = default;

void TraceRecorder::registerCallbacks(int pluginHandle)
{
    _plugin_registercallback(pluginHandle, CB_TRACEEXECUTE,
                             reinterpret_cast<CBPLUGIN>(cbTraceExecute));
    _plugin_registercallback(pluginHandle, CB_BREAKPOINT,
                             reinterpret_cast<CBPLUGIN>(cbBreakpoint));
    _plugin_registercallback(pluginHandle, CB_STARTTRACE,
                             reinterpret_cast<CBPLUGIN>(cbStartTrace));
    _plugin_registercallback(pluginHandle, CB_STOPTRACE,
                             reinterpret_cast<CBPLUGIN>(cbStopTrace));
    // CB_STOPDEBUG 不在此注册，由 plugin_callbacks::cbStopDebug 统一分发后调用
    // TraceRecorder::onStopDebug() / CallStackTracer::onStopDebug()。
    XAI_LOG_INFO("TraceRecorder: callbacks registered");
}

void TraceRecorder::unregisterCallbacks(int pluginHandle)
{
    _plugin_unregistercallback(pluginHandle, CB_TRACEEXECUTE);
    _plugin_unregistercallback(pluginHandle, CB_BREAKPOINT);
    _plugin_unregistercallback(pluginHandle, CB_STARTTRACE);
    _plugin_unregistercallback(pluginHandle, CB_STOPTRACE);
    // CB_STOPDEBUG 同步移除——由 plugin_callbacks::unregisterCallbacks 管理。
}

// ====== 启动控制 ======

bool TraceRecorder::startTargeted(const TraceTarget& tgt)
{
    if (!DbgIsDebugging()) {
        XAI_LOG_WARN("startTargeted: not debugging");
        return false;
    }
    if (tgt.entryHit == 0) {
        XAI_LOG_WARN("startTargeted: entryHit==0");
        return false;
    }

    // 先清理旧状态
    stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        target_  = tgt;
        mode_    = TraceMode::Targeted;
        entrySp_ = 0;
    }
    armed_.store(true);
    recording_.store(false);

    // 下硬件断点（执行型）
    char cmd[96];
    std::snprintf(cmd, sizeof(cmd), "bph 0x%llX,x",
                  static_cast<unsigned long long>(tgt.entryHit));
    DbgCmdExec(cmd);

    XAI_LOG_INFO("TraceRecorder: targeted ARMED entry=0x{:x} func=[0x{:x},0x{:x}) label='{}'",
                 tgt.entryHit, tgt.funcStart, tgt.funcEnd, tgt.label);

    notifyState();
    startTickMs_ = nowMs();
    return true;
}

void TraceRecorder::startGlobalActive()
{
    if (!DbgIsDebugging()) {
        XAI_LOG_WARN("startGlobalActive: not debugging");
        return;
    }
    stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        mode_ = TraceMode::Global;
        target_ = {};
        entrySp_ = 0;
    }
    armed_.store(false);
    recording_.store(true);
    startTickMs_ = nowMs();
    DbgCmdExec("TraceIntoConditional 0");
    XAI_LOG_INFO("TraceRecorder: global active recording started");
    notifyState();
}

void TraceRecorder::startGlobalPassive()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        mode_ = TraceMode::Global;
        target_ = {};
        entrySp_ = 0;
    }
    armed_.store(false);
    recording_.store(true);
    startTickMs_ = nowMs();
    XAI_LOG_INFO("TraceRecorder: global passive listening enabled");
    notifyState();
}

void TraceRecorder::stop()
{
    bool wasArmed     = armed_.exchange(false);
    bool wasRecording = recording_.exchange(false);

    if (wasRecording) {
        DbgCmdExec("StopTrace");
    }
    if (wasArmed) {
        // 拆掉刚下的硬件断点
        TraceTarget t;
        {
            std::lock_guard<std::mutex> lk(mu_);
            t = target_;
        }
        if (t.entryHit) {
            char cmd[96];
            std::snprintf(cmd, sizeof(cmd), "bphc 0x%llX",
                          static_cast<unsigned long long>(t.entryHit));
            DbgCmdExec(cmd);
        }
    }
    if (wasArmed || wasRecording) {
        XAI_LOG_INFO("TraceRecorder: stopped (armed={}, recording={})",
                     wasArmed, wasRecording);
        notifyState();
    }
}

void TraceRecorder::clear()
{
    std::lock_guard<std::mutex> lk(mu_);
    events_.clear();
    seqGen_.store(0);
    notifyAccum_ = 0;
}

TraceTarget TraceRecorder::target() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return target_;
}

std::size_t TraceRecorder::eventCount() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return events_.size();
}

std::vector<TraceEvent> TraceRecorder::snapshot() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return std::vector<TraceEvent>(events_.begin(), events_.end());
}

void TraceRecorder::setNotify(NotifyFn fn, std::size_t coalesce)
{
    std::lock_guard<std::mutex> lk(mu_);
    notify_ = std::move(fn);
    notifyCoalesce_ = coalesce > 0 ? coalesce : kNotifyDefaultCoalesce;
    notifyAccum_ = 0;
}

void TraceRecorder::setStateNotify(StateFn fn)
{
    std::lock_guard<std::mutex> lk(mu_);
    stateNotify_ = std::move(fn);
}

void TraceRecorder::notifyState()
{
    StateFn fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fn = stateNotify_;
    }
    if (fn) fn();
}

// ====== 解析工具 ======

uint64_t TraceRecorder::resolveAddressOrName(const std::string& input)
{
    if (input.empty()) return 0;
    // 去前后空白
    std::string s;
    s.reserve(input.size());
    for (char c : input) {
        if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(c);
    }
    if (s.empty()) return 0;

    // 直接交给 x64dbg 表达式求值器：支持 0x... / 函数名 / module.symbol / 表达式
    duint v = DbgValFromString(s.c_str());
    return static_cast<uint64_t>(v);
}

bool TraceRecorder::normalizeToFunction(uint64_t va, uint64_t* start, uint64_t* end)
{
    duint s = 0, e = 0;
    if (DbgFunctionGet(static_cast<duint>(va), &s, &e)) {
        if (start) *start = static_cast<uint64_t>(s);
        if (end)   *end   = static_cast<uint64_t>(e);
        return true;
    }
    return false;
}

// ====== 内部 ======

void TraceRecorder::pushEvent(TraceEvent ev)
{
    NotifyFn   pendingFn;
    std::size_t pendingCount = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (events_.size() >= capacity_) events_.pop_front();
        events_.push_back(std::move(ev));
        ++notifyAccum_;
        if (notify_ && notifyAccum_ >= notifyCoalesce_) {
            pendingFn    = notify_;
            pendingCount = events_.size();
            notifyAccum_ = 0;
        }
    }
    if (pendingFn) pendingFn(pendingCount);
}

std::string TraceRecorder::resolveSymbol(uint64_t va)
{
    if (va == 0) return {};

    char modBuf[MAX_MODULE_SIZE]   = {0};
    char labelBuf[MAX_LABEL_SIZE]  = {0};
    bool hasMod = DbgGetModuleAt(static_cast<duint>(va), modBuf);
    bool hasLab = DbgGetLabelAt(static_cast<duint>(va), SEG_DEFAULT, labelBuf);

    if (hasMod && hasLab && labelBuf[0]) {
        return std::string(modBuf) + "." + labelBuf;
    }
    if (hasLab && labelBuf[0]) {
        return std::string(labelBuf);
    }
    if (hasMod && modBuf[0]) {
        char tmp[MAX_MODULE_SIZE + 32];
        duint base = DbgModBaseFromName(modBuf);
        if (base && va >= base) {
            std::snprintf(tmp, sizeof(tmp), "%s+0x%llX",
                          modBuf, static_cast<unsigned long long>(va - base));
        } else {
            std::snprintf(tmp, sizeof(tmp), "%s", modBuf);
        }
        return std::string(tmp);
    }
    return {};
}

// ====== 回调实现 ======

void TraceRecorder::onBreakpoint(uint64_t bpAddr)
{
    if (!armed_.load()) return;

    TraceTarget t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        t = target_;
    }
    if (bpAddr != t.entryHit) return;  // 不是我们的断点

    // 记录进入时 RSP，作为返回判定阈值
    entrySp_ = static_cast<uint64_t>(Script::Register::GetCSP());
    stepsSinceStart_ = 0;
    entryModName_[0] = 0;
    DbgGetModuleAt(static_cast<duint>(t.entryHit), entryModName_);

    // 拆断点（一次性）
    char cmd[96];
    std::snprintf(cmd, sizeof(cmd), "bphc 0x%llX",
                  static_cast<unsigned long long>(t.entryHit));
    DbgCmdExec(cmd);

    armed_.store(false);
    recording_.store(true);

    // 记录一条"虚拟 Call"事件：把目标函数本身作为根
    TraceEvent ev;
    ev.seq         = seqGen_.fetch_add(1) + 1;
    ev.timestampMs = nowMs() - startTickMs_;
    ev.kind        = TraceEventKind::Call;
    ev.caller      = 0;
    ev.callee      = t.entryHit;
    ev.sp          = entrySp_;
    ev.calleeSym   = t.label.empty() ? resolveSymbol(t.entryHit) : t.label;
    pushEvent(std::move(ev));

    // 启动 trace
    // 注意：cbBreakpoint 在调试器"已暂停"上下文中调用，必须显式发 run，
    // 否则 TraceIntoConditional 会等用户按 F9 才生效，而此时函数可能早已被
    // 后续的"普通继续"跑完，导致一个 trace 事件都收不到。
    DbgCmdExec("TraceIntoConditional 0");
    DbgCmdExec("run");
    XAI_LOG_INFO("TraceRecorder: target HIT 0x{:x}, sp=0x{:x}, mod='{}', trace started + run",
                 t.entryHit, entrySp_, entryModName_);
    notifyState();
}

void TraceRecorder::onTraceExecute(uint64_t cip)
{
    if (!recording_.load()) return;
    if (cip == 0) return;

    BASIC_INSTRUCTION_INFO info{};
    DbgDisasmFastAt(static_cast<duint>(cip), &info);

    ++stepsSinceStart_;

    // === Targeted 模式：返回判定 ===
    // 至少跑 2 步后才允许触发，避免第一条指令时 RSP==entrySp 的误判。
    if (mode_ == TraceMode::Targeted && entrySp_ != 0 && stepsSinceStart_ >= 2) {
        uint64_t curSp = static_cast<uint64_t>(Script::Register::GetCSP());

        // 返回判定策略：
        //  - 已知函数边界 [start,end)：要求 CIP 跳出范围 AND SP 严格抬高
        //  - funcEnd 未知（用户自定义函数 / 系统 dll 导出符号）：仅靠 SP 严格抬高判定
        //    （CIP 模块判定对用户函数有害——返回到调用方时 CIP 仍在同模块，
        //     永远卡在"模块内"导致 trace 停不下来）
        TraceTarget t;
        {
            std::lock_guard<std::mutex> lk(mu_);
            t = target_;
        }
        // SP 必须严格高于（而非等于），保证 ret 已经执行
        bool spReturned = (curSp > entrySp_);
        bool shouldStop = false;
        if (t.funcStart != 0 && t.funcEnd != 0) {
            bool cipOut = (cip < t.funcStart || cip >= t.funcEnd);
            shouldStop = cipOut && spReturned;
        } else {
            shouldStop = spReturned;
        }

        if (shouldStop) {
            recording_.store(false);
            DbgCmdExec("StopTrace");
            XAI_LOG_INFO("TraceRecorder: target RETURNED (cip=0x{:x} sp=0x{:x} entrySp=0x{:x} steps={})",
                         cip, curSp, entrySp_, stepsSinceStart_);
            notifyState();
            return;
        }
    }

    // === call / ret 识别 ===
    // info.call 对部分间接 call（如 call rax / call [reg]）可能不置位，
    // 因此对助记符也做一次前缀匹配兜底。
    bool isCall = info.call;
    bool isRet  = false;
    {
        const char* s = info.instruction;
        while (*s == ' ') ++s;
        if (!isCall) {
            // "call"
            if ((s[0] == 'c' || s[0] == 'C') &&
                (s[1] == 'a' || s[1] == 'A') &&
                (s[2] == 'l' || s[2] == 'L') &&
                (s[3] == 'l' || s[3] == 'L')) {
                isCall = true;
            }
        }
        if (!isCall) {
            // "ret" / "retn" / "retf" / "iret"
            if ((s[0] == 'r' || s[0] == 'R') &&
                (s[1] == 'e' || s[1] == 'E') &&
                (s[2] == 't' || s[2] == 'T')) {
                isRet = true;
            } else if ((s[0] == 'i' || s[0] == 'I') &&
                       (s[1] == 'r' || s[1] == 'R') &&
                       (s[2] == 'e' || s[2] == 'E') &&
                       (s[3] == 't' || s[3] == 'T')) {
                isRet = true;
            }
        }
    }

    // 调试日志：每 256 步打印一次心跳，便于诊断 trace 是否在跑
    if ((stepsSinceStart_ & 0xFF) == 1) {
        uint64_t hbSp = static_cast<uint64_t>(Script::Register::GetCSP());
        int64_t spDelta = static_cast<int64_t>(hbSp) - static_cast<int64_t>(entrySp_);
        XAI_LOG_DEBUG("TraceRecorder: step #{} cip=0x{:x} mnem='{}' call={} ret={} sp=0x{:x} d={:+d}",
                      stepsSinceStart_, cip, info.instruction, isCall, isRet, hbSp, spDelta);
    }

    if (!isCall && !isRet) return;

    TraceEvent ev;
    ev.seq         = seqGen_.fetch_add(1) + 1;
    ev.timestampMs = nowMs() - startTickMs_;
    ev.sp          = static_cast<uint64_t>(Script::Register::GetCSP());
    ev.caller      = cip;

    if (isCall) {
        ev.kind      = TraceEventKind::Call;
        ev.callee    = static_cast<uint64_t>(info.addr);
        ev.callerSym = resolveSymbol(ev.caller);
        ev.calleeSym = resolveSymbol(ev.callee);
    } else {
        ev.kind      = TraceEventKind::Ret;
        ev.callee    = 0;
        ev.callerSym = resolveSymbol(ev.caller);
    }
    pushEvent(std::move(ev));
}

void TraceRecorder::onStartTrace()
{
    // Targeted 模式不需要响应；Global 模式被动监听用户 trace
    if (mode_ == TraceMode::Global && !recording_.load()) {
        recording_.store(true);
        notifyState();
    }
}

void TraceRecorder::onStopTrace()
{
    if (recording_.exchange(false)) {
        XAI_LOG_INFO("TraceRecorder: CB_STOPTRACE received");
        notifyState();
    }
}

void TraceRecorder::onStopDebug()
{
    // 调试停止：清理一切运行期状态
    armed_.store(false);
    recording_.store(false);
    entrySp_ = 0;
    stepsSinceStart_ = 0;
    entryModName_[0] = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        target_ = {};
    }
    notifyState();
}

}  // namespace x64ai