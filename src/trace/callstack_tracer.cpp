// trace/callstack_tracer.cpp
#include "trace/callstack_tracer.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>

#include "_plugins.h"
#include "bridgemain.h"
#include "_dbgfunctions.h"

#include "util/logging.h"

namespace x64ai {

namespace {

uint64_t nowMs()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string resolveSymbol(uint64_t va)
{
    if (va == 0) return {};
    char modBuf[MAX_MODULE_SIZE]  = {0};
    char labBuf[MAX_LABEL_SIZE]   = {0};
    bool hasMod = DbgGetModuleAt(static_cast<duint>(va), modBuf);
    bool hasLab = DbgGetLabelAt(static_cast<duint>(va), SEG_DEFAULT, labBuf);
    if (hasMod && hasLab && labBuf[0]) {
        return std::string(modBuf) + "." + labBuf;
    }
    if (hasLab && labBuf[0]) {
        return std::string(labBuf);
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

// FNV-1a 64
uint64_t fnv1a64(const void* data, std::size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

}  // namespace

CallStackTracer& CallStackTracer::instance()
{
    static CallStackTracer inst;
    return inst;
}

CallStackTracer::CallStackTracer() = default;

bool CallStackTracer::armWith(uint64_t addr, const std::string& label, uint32_t maxSamples)
{
    if (!DbgIsDebugging()) {
        XAI_LOG_WARN("CallStackTracer::armWith: not debugging");
        return false;
    }
    if (addr == 0) {
        XAI_LOG_WARN("CallStackTracer::armWith: addr==0");
        return false;
    }

    // 清旧状态
    stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        samples_.clear();
        label_       = label.empty() ? resolveSymbol(addr) : label;
        maxSamples_  = (maxSamples == 0) ? 32 : maxSamples;
        totalHits_   = 0;
        seqGen_      = 0;
        startTickMs_ = nowMs();
    }
    armedAddr_.store(addr);
    armed_.store(true);

    // 下软断点（与 TraceRecorder 的硬件断点不冲突；多个 caller 反查更适合 SetBPX）
    char cmd[96];
    std::snprintf(cmd, sizeof(cmd), "bp 0x%llX",
                  static_cast<unsigned long long>(addr));
    DbgCmdExec(cmd);

    XAI_LOG_INFO("CallStackTracer: ARMED 0x{:x} label='{}' max={}",
                 addr, label_, maxSamples_);
    return true;
}

void CallStackTracer::disarm()
{
    if (!armed_.exchange(false)) return;
    uint64_t addr = armedAddr_.exchange(0);
    if (addr) {
        char cmd[96];
        std::snprintf(cmd, sizeof(cmd), "bc 0x%llX",
                      static_cast<unsigned long long>(addr));
        DbgCmdExec(cmd);
    }
}

void CallStackTracer::stop()
{
    bool wasArmed = armed_.load();
    disarm();
    if (wasArmed) {
        XAI_LOG_INFO("CallStackTracer: stopped");
    }
}

void CallStackTracer::clear()
{
    std::lock_guard<std::mutex> lk(mu_);
    samples_.clear();
    totalHits_ = 0;
    seqGen_    = 0;
}

std::string CallStackTracer::label() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return label_;
}

uint32_t CallStackTracer::sampleCount() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return totalHits_;
}

uint32_t CallStackTracer::uniqueCount() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<uint32_t>(samples_.size());
}

std::vector<CallStackSample> CallStackTracer::snapshot() const
{
    std::lock_guard<std::mutex> lk(mu_);
    auto v = samples_;
    std::sort(v.begin(), v.end(), [](const CallStackSample& a, const CallStackSample& b){
        if (a.hits != b.hits) return a.hits > b.hits;
        return a.firstSeq < b.firstSeq;
    });
    return v;
}

uint64_t CallStackTracer::hashFrames(const std::vector<CallStackFrame>& f)
{
    // 仅以 from/to/addr 三元组参与 hash（symbol 与 PDB 加载顺序相关，不稳定）
    std::vector<uint64_t> buf;
    buf.reserve(f.size() * 3);
    for (const auto& fr : f) {
        buf.push_back(fr.addr);
        buf.push_back(fr.from);
        buf.push_back(fr.to);
    }
    return fnv1a64(buf.data(), buf.size() * sizeof(uint64_t));
}

void CallStackTracer::onBreakpoint(uint64_t bpAddr)
{
    if (!armed_.load()) return;
    if (bpAddr != armedAddr_.load()) return;

    captureOnce();

    NotifyFn fn;
    bool reachedMax = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fn = notify_;
        reachedMax = (totalHits_ >= maxSamples_);
    }

    if (reachedMax) {
        disarm();
        XAI_LOG_INFO("CallStackTracer: reached max samples, auto-disarmed");
    }

    // 必须显式 run，否则会卡在断点处等用户 F9
    DbgCmdExec("run");

    if (fn) fn();
}

void CallStackTracer::onStopDebug()
{
    armed_.store(false);
    armedAddr_.store(0);
    // 不清 samples_：用户调试停止后仍可查看最后一次结果
}

void CallStackTracer::setNotify(NotifyFn fn)
{
    std::lock_guard<std::mutex> lk(mu_);
    notify_ = std::move(fn);
}

void CallStackTracer::captureOnce()
{
    auto* fns = DbgFunctions();
    if (!fns || !fns->GetCallStack) {
        XAI_LOG_WARN("CallStackTracer: DbgFunctions->GetCallStack unavailable");
        return;
    }

    DBGCALLSTACK cs{};
    fns->GetCallStack(&cs);

    std::vector<CallStackFrame> frames;
    frames.reserve(cs.total > 0 ? static_cast<std::size_t>(cs.total) : 0);
    for (int i = 0; i < cs.total; ++i) {
        const auto& e = cs.entries[i];
        CallStackFrame f;
        f.addr = static_cast<uint64_t>(e.addr);
        f.from = static_cast<uint64_t>(e.from);
        f.to   = static_cast<uint64_t>(e.to);
        if (e.comment[0]) {
            f.symbol = e.comment;
        } else {
            // fallback：用 from 解析
            f.symbol = resolveSymbol(f.from);
        }
        frames.push_back(std::move(f));
    }
    if (cs.entries) {
        BridgeFree(cs.entries);
    }

    if (frames.empty()) {
        XAI_LOG_WARN("CallStackTracer: empty callstack on hit 0x{:x}", armedAddr_.load());
        return;
    }

    uint64_t h = hashFrames(frames);

    std::lock_guard<std::mutex> lk(mu_);
    ++totalHits_;
    for (auto& s : samples_) {
        if (s.frames.size() == frames.size() && hashFrames(s.frames) == h) {
            ++s.hits;
            return;
        }
    }
    CallStackSample ns;
    ns.frames      = std::move(frames);
    ns.hits        = 1;
    ns.firstSeq    = ++seqGen_;
    ns.timestampMs = nowMs() - startTickMs_;
    samples_.push_back(std::move(ns));

    XAI_LOG_DEBUG("CallStackTracer: new unique stack #{} (total hits={}, depth={})",
                  samples_.size(), totalHits_, samples_.back().frames.size());
}

}  // namespace x64ai
