// dbg/event_bus.cpp
#include "dbg/event_bus.h"

#include "util/logging.h"

namespace x64ai {

EventBus& EventBus::instance()
{
    static EventBus inst;
    return inst;
}

EventBus::Token EventBus::subscribe(DbgEvent ev, Handler h)
{
    if (!h) return 0;
    const Token t = ++tokenGen_;
    {
        std::lock_guard<std::mutex> lk(subMu_);
        subs_[idx(ev)].push_back(Sub{t, std::move(h)});
    }
    return t;
}

void EventBus::unsubscribe(Token t)
{
    if (t == 0) return;
    std::lock_guard<std::mutex> lk(subMu_);
    for (auto& vec : subs_) {
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if (it->token == t) { vec.erase(it); return; }
        }
    }
}

bool EventBus::waitOnce(DbgEvent ev,
                        std::chrono::milliseconds timeout,
                        DbgEventPayload* outPayload,
                        const std::atomic<bool>* cancelFlag)
{
    WaitSlot& slot = waitSlots_[idx(ev)];

    std::unique_lock<std::mutex> lk(slot.mu);
    // 记录"等待开始时已知的最大 seq"；只接受比它新的 publish
    const std::uint64_t baseSeq = slot.lastSeq;
    slot.cancelled = false;

    // 用 wait_for 切片以支持 cancelFlag 轮询（粒度 50ms 足够 agent 用）
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + timeout;
    const auto poll     = std::chrono::milliseconds(50);

    while (true) {
        if (cancelFlag && cancelFlag->load()) {
            return false;
        }
        if (slot.cancelled) {
            return false;
        }
        if (slot.lastSeq > baseSeq) {
            if (outPayload) *outPayload = slot.lastPayload;
            return true;
        }
        const auto now = clock::now();
        if (now >= deadline) return false;
        const auto step = std::min(deadline - now,
                                   std::chrono::duration_cast<clock::duration>(poll));
        slot.cv.wait_for(lk, step);
    }
}

void EventBus::publish(DbgEvent ev, const DbgEventPayload& payload)
{
    const std::uint64_t seq = ++seqGen_;
    DbgEventPayload p = payload;
    p.kind = ev;
    p.seq  = seq;

    // 唤醒 waiter
    {
        WaitSlot& slot = waitSlots_[idx(ev)];
        {
            std::lock_guard<std::mutex> lk(slot.mu);
            slot.lastPayload = p;
            slot.lastSeq     = seq;
        }
        slot.cv.notify_all();
    }

    // 拷贝 handler 列表后释放锁再调用，避免回调里再 subscribe/unsubscribe 死锁
    std::vector<Handler> handlers;
    {
        std::lock_guard<std::mutex> lk(subMu_);
        const auto& vec = subs_[idx(ev)];
        handlers.reserve(vec.size());
        for (const auto& s : vec) handlers.push_back(s.handler);
    }
    for (auto& h : handlers) {
        try {
            h(p);
        } catch (const std::exception& e) {
            XAI_LOG_ERROR("EventBus handler threw on ev={} seq={}: {}",
                          static_cast<int>(ev), seq, e.what());
        } catch (...) {
            XAI_LOG_ERROR("EventBus handler threw unknown on ev={} seq={}",
                          static_cast<int>(ev), seq);
        }
    }
}

void EventBus::cancelAllWaits()
{
    for (auto& slot : waitSlots_) {
        {
            std::lock_guard<std::mutex> lk(slot.mu);
            slot.cancelled = true;
        }
        slot.cv.notify_all();
    }
}

std::size_t EventBus::subscriberCount(DbgEvent ev) const
{
    std::lock_guard<std::mutex> lk(subMu_);
    return subs_[idx(ev)].size();
}

}  // namespace x64ai
